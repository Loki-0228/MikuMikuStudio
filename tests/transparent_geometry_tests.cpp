// ===========================================================================
// Transparent geometry ordering regressions (pure CPU, no device access).
// ===========================================================================
// Contract under test (include/mikumikustudio/transparent_geometry.hpp and
// TransparentSortPrecision from view_options.hpp):
//
//   * High orders whole faces by their centre depth, far to near, and
//     breaks an equal depth by ordinal.
//   * Standard orders the same depth key with 64 counting buckets; Low uses 16.
//     Both process far-to-near buckets in one
//     pass. A deeper face can never follow a shallower one that sits in another
//     bucket, but faces sharing a bucket keep the input order, so a nearer face
//     may well precede a farther one.
//   * All three lower tiers only reorder: every valid input face comes back
//     exactly once with all of its vertex components unchanged.
//   * Only Maximum may split; the splitting regressions below run at
//     Maximum, which carries the BSP budget the retired VeryHigh tier used to
//     carry, so their geometry coverage is unchanged.
//   * Order() returns fragments back-to-front. A fragment earlier in the
//     result is farther from the camera (larger NDC z) and is drawn first, so
//     at any one screen point the depth along the result never increases.
//   * The input vector is the single sequence across all draws and models.
//     Coplanar faces have no depth order at all and keep that sequence.
//   * Fragments produced by splitting carry the draw/ordinal of the triangle
//     they came from, and every input triangle keeps appearing.
//   * A split vertex interpolates p and inverseW linearly along the NDC edge,
//     and data perspective-correctly.
//
// Every expected value below is derived from analytic plane/edge geometry;
// none of them is copied from the implementation.
// ===========================================================================
#include "mikumikustudio/transparent_geometry.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using mikumikustudio::TransparentSortPrecision;
using mikumikustudio::transparent_geometry::Order;
using mikumikustudio::transparent_geometry::Statistics;
using mikumikustudio::transparent_geometry::Triangle;
using mikumikustudio::transparent_geometry::Vertex;

namespace {

int failures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

void Check(bool ok, const char* what) {
    if (!ok) Fail(what);
}

Vertex MakeVertex(double x, double y, double z, double inverseW) {
    Vertex v;
    v.p[0] = x;
    v.p[1] = y;
    v.p[2] = z;
    v.inverseW = inverseW;
    return v;
}

Triangle MakeTriangle(const Vertex& a, const Vertex& b, const Vertex& c,
                      std::uint32_t draw, std::uint32_t ordinal) {
    Triangle t;
    t.v[0] = a;
    t.v[1] = b;
    t.v[2] = c;
    t.draw = draw;
    t.ordinal = ordinal;
    return t;
}

// All 24 data components get distinct values per triangle and per vertex, so a
// wrong interpolation formula cannot hide behind uniform data.
void FillData(Triangle& t, int tag) {
    for (int v = 0; v < 3; ++v) {
        for (int k = 0; k < 24; ++k) {
            t.v[v].data[k] =
                static_cast<float>((tag * 7 + v * 3 + 1) * 0.117 + k * 0.0311);
        }
    }
}

bool SameTag(const Triangle& a, const Triangle& b) {
    return a.draw == b.draw && a.ordinal == b.ordinal;
}

// The depth key the tier contract orders by: the triangle's centre depth. The
// tests below carry the sum of the three NDC z values instead, which is that
// centre times three and therefore orders identically.
double TriangleDepth(const Triangle& t) {
    return t.v[0].p[2] + t.v[1].p[2] + t.v[2].p[2];
}

// Full identity of one fragment, including every vertex component. The
// reordering tiers move triangles around without touching them, so an exact
// comparison is the right strength here, and it also catches any attribute
// that a reordering pass might have rebuilt.
bool SameFragment(const Triangle& a, const Triangle& b) {
    if (a.draw != b.draw || a.ordinal != b.ordinal) return false;
    for (int i = 0; i < 3; ++i) {
        if (a.v[i].inverseW != b.v[i].inverseW) return false;
        for (int k = 0; k < 3; ++k) {
            if (a.v[i].p[k] != b.v[i].p[k]) return false;
        }
        for (int k = 0; k < 24; ++k) {
            if (a.v[i].data[k] != b.v[i].data[k]) return false;
        }
    }
    return true;
}

// One-to-one preservation: the result has the input's size and every input
// fragment appears in it exactly once, unchanged. This is the whole obligation
// of a tier that only reorders.
void CheckExactReorder(const std::vector<Triangle>& input,
                       const std::vector<Triangle>& out, const char* what) {
    if (out.size() != input.size()) {
        Fail(what);
        return;
    }
    std::vector<bool> matched(input.size(), false);
    for (const Triangle& f : out) {
        std::size_t hit = input.size();
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (!matched[i] && SameFragment(f, input[i])) {
                hit = i;
                break;
            }
        }
        if (hit == input.size()) {
            Fail(what);
            return;
        }
        matched[hit] = true;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (!matched[i]) {
            Fail(what);
            return;
        }
    }
}

// Pairs where a nearer fragment is emitted before a farther one. A bucket order
// over separated depths has none; a bucket order that keeps the input order
// inside a bucket may have many.
int Inversions(const std::vector<Triangle>& out) {
    int bad = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (TriangleDepth(out[i]) < TriangleDepth(out[j]) - 1e-12) ++bad;
        }
    }
    return bad;
}

// Barycentric coordinates of the screen point (x, y) inside the triangle's NDC
// footprint. Returns false when the footprint has zero area. For a point that
// lies in the triangle's NDC plane these weights also reproduce p[2].
bool FootprintWeights(const Triangle& t, double x, double y, double w[3]) {
    const double x0 = t.v[0].p[0], y0 = t.v[0].p[1];
    const double x1 = t.v[1].p[0], y1 = t.v[1].p[1];
    const double x2 = t.v[2].p[0], y2 = t.v[2].p[1];
    const double det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (!(det > 0.0 || det < 0.0)) return false;
    w[0] = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / det;
    w[1] = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / det;
    w[2] = 1.0 - w[0] - w[1];
    return true;
}

struct Cover {
    std::size_t index = 0;
    std::uint32_t draw = 0;
    double depth = 0.0;
};

// Rebuilds which result fragments cover the screen point (x, y) and which NDC
// depth each of them has there. The returned order is the draw order.
std::vector<Cover> CoversAt(const std::vector<Triangle>& out, double x, double y) {
    std::vector<Cover> covers;
    for (std::size_t i = 0; i < out.size(); ++i) {
        double w[3];
        if (!FootprintWeights(out[i], x, y, w)) continue;
        if (w[0] < -1e-9 || w[1] < -1e-9 || w[2] < -1e-9) continue;
        Cover c;
        c.index = i;
        c.draw = out[i].draw;
        c.depth = w[0] * out[i].v[0].p[2] + w[1] * out[i].v[1].p[2] +
                  w[2] * out[i].v[2].p[2];
        covers.push_back(c);
    }
    return covers;
}

// Back-to-front result: fragment i is drawn before fragment j (i < j), so the
// depth seen at one screen point may never grow along the result. Counts pairs
// where a nearer fragment is drawn before a farther one.
int DepthOrderViolations(const std::vector<Cover>& covers) {
    int bad = 0;
    for (std::size_t i = 0; i < covers.size(); ++i) {
        for (std::size_t j = i + 1; j < covers.size(); ++j) {
            if (covers[i].depth < covers[j].depth - 1e-6) ++bad;
        }
    }
    return bad;
}

double FootprintArea(const Triangle& t) {
    const double ax = t.v[1].p[0] - t.v[0].p[0], ay = t.v[1].p[1] - t.v[0].p[1];
    const double bx = t.v[2].p[0] - t.v[0].p[0], by = t.v[2].p[1] - t.v[0].p[1];
    return std::fabs(ax * by - ay * bx) * 0.5;
}

double TotalArea(const std::vector<Triangle>& list) {
    double sum = 0.0;
    for (const Triangle& t : list) sum += FootprintArea(t);
    return sum;
}

// ---------------------------------------------------------------------------
// Intersecting faces: correct far/near order at a left and a right sample.
// ---------------------------------------------------------------------------
// Two faces cross in an X. Face A lies in the plane z = 0.5 + 0.35*x, face B
// in z = 0.5 - 0.35*x; the planes meet on the line x = 0. Analytic NDC depth at
// the two sample points:
//
//   sample (-0.5, -0.2):  A = 0.325 (near),  B = 0.675 (far)
//   sample ( 0.5, -0.2):  A = 0.675 (far),   B = 0.325 (near)
//
// Back-to-front order therefore needs B's pieces before A's on the left sample
// and A's pieces before B's on the right one. No order of the two whole
// triangles satisfies both, so a correct result must split them and order the
// pieces per sample point.
void TestIntersectingFaces() {
    Triangle a = MakeTriangle(MakeVertex(-1.0, -0.7, 0.15, 1.0),
                              MakeVertex(1.0, -0.7, 0.85, 1.0),
                              MakeVertex(0.0, 0.7, 0.5, 1.0), 1, 10);
    Triangle b = MakeTriangle(MakeVertex(-1.0, -0.7, 0.85, 1.0),
                              MakeVertex(1.0, -0.7, 0.15, 1.0),
                              MakeVertex(0.0, 0.7, 0.5, 1.0), 2, 20);
    std::vector<Triangle> input{a, b};
    Statistics stats;
    const std::vector<Triangle> out =
        Order(input, TransparentSortPrecision::Maximum, stats);

    Check(stats.inputTriangles == 2, "intersecting: stats.inputTriangles");
    Check(stats.outputTriangles == out.size(), "intersecting: stats.outputTriangles");
    Check(out.size() > input.size(), "intersecting: crossing faces must be split");
    Check(std::fabs(TotalArea(out) - TotalArea(input)) < 1e-9,
          "intersecting: no face area may be lost");

    struct Sample {
        double x, y, depthA, depthB;
    };
    const Sample samples[2] = {
        {-0.5, -0.2, 0.325, 0.675},
        {0.5, -0.2, 0.675, 0.325},
    };
    for (const Sample& s : samples) {
        const std::vector<Cover> covers = CoversAt(out, s.x, s.y);
        Check(covers.size() >= 2, "intersecting: both faces must cover the sample");
        bool sawA = false, sawB = false;
        for (const Cover& c : covers) {
            if (c.draw == 1) {
                sawA = true;
                Check(std::fabs(c.depth - s.depthA) < 1e-9,
                      "intersecting: analytic depth of face A");
            } else if (c.draw == 2) {
                sawB = true;
                Check(std::fabs(c.depth - s.depthB) < 1e-9,
                      "intersecting: analytic depth of face B");
            }
        }
        Check(sawA && sawB, "intersecting: both faces must cover the sample");
        Check(DepthOrderViolations(covers) == 0,
              "intersecting: back-to-front order at sample");
    }
}

// ---------------------------------------------------------------------------
// Coplanar faces keep their ordinal order.
// ---------------------------------------------------------------------------
// Coplanar faces have no depth order at all, so the result must keep the input
// sequence instead of reordering them. Here the input sequence, the draws and
// the ordinals all ascend together, so any of those readings agrees.
void TestCoplanarStableOrder() {
    std::vector<Triangle> input;
    for (int i = 0; i < 4; ++i) {
        const double x0 = -0.8 + 0.15 * i, y0 = -0.8 + 0.15 * i;
        input.push_back(MakeTriangle(MakeVertex(x0, y0, 0.5, 1.0),
                                     MakeVertex(x0 + 1.2, y0, 0.5, 1.0),
                                     MakeVertex(x0, y0 + 1.2, 0.5, 1.0),
                                     static_cast<std::uint32_t>(5 + i),
                                     static_cast<std::uint32_t>(100 + i)));
    }
    Statistics stats;
    const std::vector<Triangle> out =
        Order(input, TransparentSortPrecision::Maximum, stats);

    Check(out.size() == input.size(), "coplanar: one fragment per coplanar face");
    for (std::size_t i = 0; i < out.size() && i < input.size(); ++i) {
        Check(SameTag(out[i], input[i]), "coplanar: input ordinal order preserved");
    }
}

// ---------------------------------------------------------------------------
// Draw numbers survive splitting.
// ---------------------------------------------------------------------------
// The X pair is joined by a third face that crosses both, so fragments are
// definitely produced. Every result fragment must carry the draw/ordinal of
// its source triangle, and every input triangle must still be present.
void TestDrawNumbersPreserved() {
    Triangle a = MakeTriangle(MakeVertex(-1.0, -0.7, 0.15, 1.0),
                              MakeVertex(1.0, -0.7, 0.85, 1.0),
                              MakeVertex(0.0, 0.7, 0.5, 1.0), 3, 5);
    Triangle b = MakeTriangle(MakeVertex(-1.0, -0.7, 0.85, 1.0),
                              MakeVertex(1.0, -0.7, 0.15, 1.0),
                              MakeVertex(0.0, 0.7, 0.5, 1.0), 7, 6);
    Triangle c = MakeTriangle(MakeVertex(-0.6, -0.2, 0.2, 1.0),
                              MakeVertex(0.9, -0.4, 0.75, 1.0),
                              MakeVertex(0.1, 0.8, 0.55, 1.0), 11, 7);
    std::vector<Triangle> input{a, b, c};
    Statistics stats;
    const std::vector<Triangle> out =
        Order(input, TransparentSortPrecision::Maximum, stats);

    Check(out.size() >= input.size(), "draw: no input triangle may disappear");
    std::size_t seen[3] = {0, 0, 0};
    for (const Triangle& f : out) {
        int source = -1;
        for (int i = 0; i < 3; ++i) {
            if (SameTag(f, input[static_cast<std::size_t>(i)])) source = i;
        }
        if (source < 0) {
            Fail("draw: fragment must keep a source draw/ordinal");
            continue;
        }
        ++seen[source];
    }
    for (int i = 0; i < 3; ++i) {
        Check(seen[static_cast<std::size_t>(i)] >= 1,
              "draw: every input triangle must survive");
    }
}

// ---------------------------------------------------------------------------
// Cyclic occlusion ring: A occludes B, B occludes C, C occludes A.
// ---------------------------------------------------------------------------
// Three faces are copies of one template under 120 degree rotations about the
// z axis (template scaled into the NDC box). Face A lies in the plane
// z = 0.5 + 0.2*x; the rotated copies lie in z = 0.5 - 0.1*x + sqrt(3)*0.1*y
// (face B) and z = 0.5 - 0.1*x - sqrt(3)*0.1*y (face C). Rotation about z keeps
// the template depth, so each face keeps the z of its template vertices.
//
// The three faces never intersect. For the pair A/B the depth difference is
// zA - zB = 0.3*x - sqrt(3)*0.1*y, which vanishes only on the line
// y = sqrt(3)*x; the two footprints overlap in a region around the sample
// (-0.54, 0), where y - sqrt(3)*x is about 0.94, so the difference stays at
// -0.162 there and the faces stay apart. The 120 degree symmetry gives the
// same margin to the other two pairs, and the three pairwise overlaps are
// disjoint, so no screen point belongs to all three faces. The occlusion still
// forms the ring A>B>C>A, so no single order of the three whole faces can be
// correct at all three sample points.
//
// Analytic NDC depth at the samples (near = 0.392 = 0.5 + 0.2*(-0.54),
// far = 0.554 = 0.5 + 0.2*0.27):
//
//   X = (-0.54,  0.0000): A = 0.392, B = 0.554  -> A occludes B
//   Y = ( 0.27,  0.4677): C = 0.392, A = 0.554  -> C occludes A
//   Z = ( 0.27, -0.4677): B = 0.392, C = 0.554  -> B occludes C
//
// so a back-to-front result must draw B before A at X, A before C at Y, and C
// before B at Z.
void TestCyclicOcclusionRing() {
    const double c120 = -0.5;
    const double s120 = 0.86602540378443864676;  // sqrt(3)/2
    const double tmpl[3][2] = {{-0.9, -0.225}, {0.765, 0.558}, {-0.225, 0.9}};
    double rot1[3][2], rot2[3][2];
    for (int i = 0; i < 3; ++i) {
        rot1[i][0] = tmpl[i][0] * c120 - tmpl[i][1] * s120;
        rot1[i][1] = tmpl[i][0] * s120 + tmpl[i][1] * c120;
        rot2[i][0] = rot1[i][0] * c120 - rot1[i][1] * s120;
        rot2[i][1] = rot1[i][0] * s120 + rot1[i][1] * c120;
    }
    std::vector<Triangle> input;
    for (int face = 0; face < 3; ++face) {
        const double(*xy)[2] = face == 0 ? tmpl : (face == 1 ? rot1 : rot2);
        Vertex v[3];
        for (int i = 0; i < 3; ++i) {
            v[i] = MakeVertex(xy[i][0], xy[i][1], 0.5 + 0.2 * tmpl[i][0], 1.0);
        }
        input.push_back(MakeTriangle(v[0], v[1], v[2],
                                     static_cast<std::uint32_t>(face + 1),
                                     static_cast<std::uint32_t>(11 * (face + 1))));
    }

    // Sample X, then its 120 and 240 degree rotations Z and Y.
    const double sx = -0.54, sy = 0.0;
    const double zx = sx * c120 - sy * s120, zy = sx * s120 + sy * c120;
    const double yx = zx * c120 - zy * s120, yy = zx * s120 + zy * c120;
    struct Sample {
        double x, y;
        std::uint32_t nearDraw, farDraw;
        double nearDepth, farDepth;
    };
    const Sample samples[3] = {
        {sx, sy, 1, 2, 0.392, 0.554},  // A occludes B
        {yx, yy, 3, 1, 0.392, 0.554},  // C occludes A
        {zx, zy, 2, 3, 0.392, 0.554},  // B occludes C
    };

    // Construct check: every sample lies in exactly the two faces of its pair,
    // and their analytic plane depths there are the stated values.
    for (const Sample& s : samples) {
        for (const Triangle& in : input) {
            double w[3];
            const bool covers = FootprintWeights(in, s.x, s.y, w) &&
                                w[0] >= -1e-12 && w[1] >= -1e-12 &&
                                w[2] >= -1e-12;
            const bool expected =
                in.draw == s.nearDraw || in.draw == s.farDraw;
            Check(covers == expected,
                  "ring: input coverage must match the sample pair");
            if (covers) {
                const double depth = w[0] * in.v[0].p[2] + w[1] * in.v[1].p[2] +
                                     w[2] * in.v[2].p[2];
                const double analytic =
                    in.draw == s.nearDraw ? s.nearDepth : s.farDepth;
                Check(std::fabs(depth - analytic) < 1e-9,
                      "ring: analytic input depth at sample");
            }
        }
    }

    Statistics stats;
    const std::vector<Triangle> out =
        Order(input, TransparentSortPrecision::Maximum, stats);

    Check(out.size() >= input.size(), "ring: no face may be lost");
    Check(std::fabs(TotalArea(out) - TotalArea(input)) < 1e-9,
          "ring: area must be conserved");
    for (const Sample& s : samples) {
        const std::vector<Cover> covers = CoversAt(out, s.x, s.y);
        Check(covers.size() >= 2, "ring: both faces must cover the sample");
        bool sawNear = false, sawFar = false;
        for (const Cover& c : covers) {
            if (c.draw == s.nearDraw) {
                sawNear = true;
                Check(std::fabs(c.depth - s.nearDepth) < 1e-6,
                      "ring: analytic near depth at sample");
            } else if (c.draw == s.farDraw) {
                sawFar = true;
                Check(std::fabs(c.depth - s.farDepth) < 1e-6,
                      "ring: analytic far depth at sample");
            } else {
                Fail("ring: a third face must not cover the sample");
            }
        }
        Check(sawNear && sawFar, "ring: both faces must cover the sample");
        Check(DepthOrderViolations(covers) == 0,
              "ring: far-to-near order at sample");
    }
}

// ---------------------------------------------------------------------------
// Perspective-correct interpolation of all 24 data components.
// ---------------------------------------------------------------------------
// Independent expectation: after the perspective divide both data*inverseW and
// 1/w are affine along the NDC edge. With barycentric weights (u, v, w) of a
// point inside its source triangle (derived from that triangle's NDC
// footprint) the values must therefore satisfy
//
//     inverseW = u*s0 + v*s1 + w*s2
//     data[k]  = (u*d0[k]*s0 + v*d1[k]*s1 + w*d2[k]*s2) / inverseW
//
// where s and d are the inverseW and data of the three source vertices. This
// holds for every split vertex regardless of how often or where the
// implementation cut the triangle.
void TestPerspectiveInterpolation() {
    Triangle a = MakeTriangle(MakeVertex(-1.0, -0.7, 0.15, 0.5),
                              MakeVertex(1.0, -0.7, 0.85, 0.87),
                              MakeVertex(0.0, 0.7, 0.5, 1.24), 3, 5);
    Triangle b = MakeTriangle(MakeVertex(-1.0, -0.7, 0.85, 1.03),
                              MakeVertex(1.0, -0.7, 0.15, 1.4),
                              MakeVertex(0.0, 0.7, 0.5, 1.77), 7, 6);
    Triangle c = MakeTriangle(MakeVertex(-0.6, -0.2, 0.2, 2.14),
                              MakeVertex(0.9, -0.4, 0.75, 2.51),
                              MakeVertex(0.1, 0.8, 0.55, 2.88), 11, 7);
    FillData(a, 0);
    FillData(b, 1);
    FillData(c, 2);
    std::vector<Triangle> input{a, b, c};
    Statistics stats;
    const std::vector<Triangle> out =
        Order(input, TransparentSortPrecision::Maximum, stats);

    Check(out.size() > input.size(), "attributes: crossing faces must be split");
    std::size_t checkedVertices = 0;
    for (const Triangle& f : out) {
        const Triangle* source = nullptr;
        for (const Triangle& in : input) {
            if (SameTag(f, in)) source = &in;
        }
        if (!source) {
            Fail("attributes: fragment without a source triangle");
            continue;
        }
        for (const Vertex& v : f.v) {
            double w[3];
            if (!FootprintWeights(*source, v.p[0], v.p[1], w)) {
                Fail("attributes: source footprint has zero area");
                continue;
            }
            if (w[0] < -1e-9 || w[1] < -1e-9 || w[2] < -1e-9) {
                Fail("attributes: vertex outside its source triangle");
                continue;
            }
            const double s = w[0] * source->v[0].inverseW +
                             w[1] * source->v[1].inverseW +
                             w[2] * source->v[2].inverseW;
            const double z = w[0] * source->v[0].p[2] + w[1] * source->v[1].p[2] +
                             w[2] * source->v[2].p[2];
            Check(std::fabs(v.p[2] - z) <= 1e-6, "attributes: NDC z interpolation");
            Check(std::fabs(v.inverseW - s) <= 1e-6 * (1.0 + std::fabs(s)),
                  "attributes: inverseW interpolation");
            double worst = 0.0;
            for (int k = 0; k < 24; ++k) {
                const double expected =
                    (w[0] * source->v[0].data[k] * source->v[0].inverseW +
                     w[1] * source->v[1].data[k] * source->v[1].inverseW +
                     w[2] * source->v[2].data[k] * source->v[2].inverseW) / s;
                const double error =
                    std::fabs(static_cast<double>(v.data[k]) - expected);
                const double allowed = 1e-5 * (1.0 + std::fabs(expected));
                if (error > allowed && error > worst) worst = error;
            }
            if (worst > 0.0) Fail("attributes: perspective data interpolation");
            ++checkedVertices;
        }
    }
    Check(checkedVertices == out.size() * 3, "attributes: every vertex checked");
}

// ---------------------------------------------------------------------------
// Standard: 64 counting buckets, far to near, across draws.
// ---------------------------------------------------------------------------
// Eight depth levels with sixteen faces each: 128 faces, far more than the 64
// buckets. Every vertex of level L carries z = 0.125*L, so that level's depth
// key is exactly 0.375*L. The levels are 0.375 apart while the input spans
// 2.625, and one bucket is 2.625/64 = 0.041 wide, so two levels can never share
// a bucket. Every cross-level pair must therefore come out far first, and
// exactly eight buckets are filled.
//
// Inside one level all depths are equal, so those faces share a bucket and must
// keep the input order. The ordinals inside a level descend while the input
// order ascends, which tells the stable bucket order apart from the ordinal
// tie-break the Standard tier applies to equal depths.
//
// Draws are 100 + k, so a single draw spans all eight levels: an order that is
// only correct inside one draw cannot pass the checks below.
void TestStandardBucketOrderAcrossDraws() {
    const int kLevels = 8;
    const int kPerLevel = 16;
    const int kVisit[8] = {3, 7, 0, 5, 2, 6, 1, 4};  // interleaves the levels
    std::vector<Triangle> input;
    for (int k = 0; k < kPerLevel; ++k) {
        const double x0 = -0.9 + 0.1 * (k % 4);
        const double y0 = -0.9 + 0.1 * (k / 4);
        for (int visit = 0; visit < kLevels; ++visit) {
            const int level = kVisit[visit];
            const double z = 0.125 * level;
            const double w = 0.5 + 0.01 * k + 0.125 * level;
            Triangle t = MakeTriangle(MakeVertex(x0, y0, z, w),
                                      MakeVertex(x0 + 0.05, y0, z, w),
                                      MakeVertex(x0, y0 + 0.05, z, w),
                                      static_cast<std::uint32_t>(100 + k),
                                      static_cast<std::uint32_t>(level * 100 + (kPerLevel - 1 - k)));
            FillData(t, level * 100 + k);
            input.push_back(t);
        }
    }

    Statistics stats;
    const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);

    Check(input.size() == 128, "low-buckets: the case must exceed the bucket count");
    Check(stats.inputTriangles == input.size(), "low-buckets: stats.inputTriangles");
    Check(stats.outputTriangles == out.size(), "low-buckets: stats.outputTriangles");
    Check(stats.splitCount == 0, "low-buckets: Low must not split a face");
    Check(!stats.limited, "low-buckets: no face may be limited out");
    Check(out.size() == input.size(), "low-buckets: no face may be added or lost");
    Check(stats.bucketCount == kLevels, "low-buckets: one bucket per depth level");
    CheckExactReorder(input, out, "low-buckets: every face and vertex survives exactly once");

    // Hand-derived expectation: levels far to near, input order inside a level.
    // The input pushes level L at k = 0..15, so inside a level the order is k
    // ascending, which is the reverse of the ordinal order.
    std::size_t at = 0;
    for (int level = kLevels - 1; level >= 0; --level) {
        for (int k = 0; k < kPerLevel; ++k) {
            const bool ok =
                at < out.size() &&
                out[at].draw == static_cast<std::uint32_t>(100 + k) &&
                out[at].ordinal == static_cast<std::uint32_t>(level * 100 + (kPerLevel - 1 - k));
            Check(ok, "low-buckets: far level first, input order inside the level");
            ++at;
        }
    }
    Check(at == out.size(), "low-buckets: the expectation covers every face");

    // A nearer face may never be emitted before a farther one that sits in
    // another bucket, whatever draws the two belong to.
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (out[i].draw == out[j].draw) continue;
            Check(TriangleDepth(out[i]) >= TriangleDepth(out[j]) - 1e-12,
                  "low-buckets: depth never increases across draws");
        }
    }
}

// ---------------------------------------------------------------------------
// High: per-triangle depth, ordinal breaks an equal depth.
// ---------------------------------------------------------------------------
// Five faces in three depth groups. The input order disagrees with both the
// depth order and the ordinal order, so sorting by input order, or ignoring the
// ordinal tie-break, cannot pass this case:
//
//   deep  depth 1.5  : ordinals 9, 3  -> 3 before 9
//   mid   depth 0.75 : ordinal  5
//   near  depth 0.0  : ordinals 7, 2  -> 2 before 7
//
// Expected: deep(3), deep(9), mid(5), near(2), near(7).
void TestHighDepthOrderAndOrdinalTies() {
    const double kDeep = 0.5, kMid = 0.25, kNear = 0.0;  // on all three vertices
    Triangle d9 = MakeTriangle(MakeVertex(-0.9, -0.9, kDeep, 1.0),
                               MakeVertex(-0.1, -0.9, kDeep, 1.0),
                               MakeVertex(-0.9, -0.1, kDeep, 1.0), 21, 9);
    Triangle n7 = MakeTriangle(MakeVertex(-0.8, -0.8, kNear, 1.0),
                               MakeVertex(0.0, -0.8, kNear, 1.0),
                               MakeVertex(-0.8, 0.0, kNear, 1.0), 22, 7);
    Triangle m5 = MakeTriangle(MakeVertex(-0.7, -0.7, kMid, 1.0),
                               MakeVertex(0.1, -0.7, kMid, 1.0),
                               MakeVertex(-0.7, 0.1, kMid, 1.0), 23, 5);
    Triangle d3 = MakeTriangle(MakeVertex(-0.6, -0.6, kDeep, 1.0),
                               MakeVertex(0.2, -0.6, kDeep, 1.0),
                               MakeVertex(-0.6, 0.2, kDeep, 1.0), 24, 3);
    Triangle n2 = MakeTriangle(MakeVertex(-0.5, -0.5, kNear, 1.0),
                               MakeVertex(0.3, -0.5, kNear, 1.0),
                               MakeVertex(-0.5, 0.3, kNear, 1.0), 25, 2);
    std::vector<Triangle> input{d9, n7, m5, d3, n2};

    Statistics stats;
    const std::vector<Triangle> out = Order(input, TransparentSortPrecision::High, stats);

    Check(stats.splitCount == 0, "high: High must not split a face");
    Check(out.size() == input.size(), "high: no face may be added or lost");
    Check(stats.bucketCount == 0, "high: High uses no counting bucket");
    CheckExactReorder(input, out, "high: every face and vertex survives exactly once");

    const std::uint32_t expected[5] = {3, 9, 5, 2, 7};
    for (int i = 0; i < 5; ++i) {
        Check(i < static_cast<int>(out.size()) &&
                  out[static_cast<std::size_t>(i)].ordinal == expected[i],
              "high: depth order with the ordinal tie-break");
    }
    Check(Inversions(out) == 0, "high: no inversion at all");

    // Standard reuses the same depth key but not the tie-break: inside one depth
    // group it keeps the input order, so the two tiers must disagree here.
    Statistics standardStats;
    const std::vector<Triangle> standard = Order(input, TransparentSortPrecision::Standard, standardStats);
    const std::uint32_t standardExpected[5] = {9, 3, 5, 7, 2};
    for (int i = 0; i < 5; ++i) {
        Check(i < static_cast<int>(standard.size()) &&
                  standard[static_cast<std::size_t>(i)].ordinal == standardExpected[i],
              "high: Standard keeps the input order inside a depth group");
    }
    bool same = out.size() == standard.size();
    for (std::size_t i = 0; same && i < out.size(); ++i) same = SameTag(out[i], standard[i]);
    Check(!same, "high: Standard and High must not agree on this input");
}

// ---------------------------------------------------------------------------
// Standard: an inversion inside one bucket, which High never produces.
// ---------------------------------------------------------------------------
// Two anchors fix the depth span at exactly [0, 1]: the far anchor has depth 1
// (one vertex at z = 1, two at z = 0) and the near anchor depth 0. One bucket is
// therefore 1/64 = 0.015625 wide. Sixty-five cluster faces carry the depths
// 0.5 + j*1e-9 for j = 0..64, a range of 6.4e-8, far less than one bucket: the
// whole cluster fits in at most two buckets, so at least 33 of its faces end up
// in the same one. The cluster is written nearest first.
//
// High sorts by depth, so its result descends and holds no inversion. Standard
// keeps the input order inside a bucket, so that bucket emits its cluster faces
// nearest first: the result must contain a nearer-before-farther pair and it
// cannot equal High's.
void TestStandardBucketInversionVersusHigh() {
    const int kCluster = 65;
    std::vector<Triangle> input;
    // Far anchor: depth = 1 + 0 + 0, the span maximum.
    input.push_back(MakeTriangle(MakeVertex(-0.5, -0.5, 1.0, 1.0),
                                 MakeVertex(0.5, -0.5, 0.0, 1.0),
                                 MakeVertex(-0.5, 0.5, 0.0, 1.0), 1, 1));
    for (int j = 0; j < kCluster; ++j) {
        const double c = 0.5 + 1e-9 * j;
        const double x0 = -0.4 + 0.001 * j;
        input.push_back(MakeTriangle(MakeVertex(x0, -0.4, c, 1.0),
                                     MakeVertex(x0 + 0.2, -0.4, 0.0, 1.0),
                                     MakeVertex(x0, -0.2, 0.0, 1.0), 2,
                                     static_cast<std::uint32_t>(100 + j)));
    }
    // Near anchor: depth = 0, the span minimum.
    input.push_back(MakeTriangle(MakeVertex(-0.5, -0.5, 0.0, 1.0),
                                 MakeVertex(0.2, -0.5, 0.0, 1.0),
                                 MakeVertex(-0.5, 0.2, 0.0, 1.0), 3, 2));

    Statistics standardStats, highStats;
    const std::vector<Triangle> standard = Order(input, TransparentSortPrecision::Standard, standardStats);
    const std::vector<Triangle> high =
        Order(input, TransparentSortPrecision::High, highStats);

    Check(standardStats.splitCount == 0, "standard-inversion: Standard must not split a face");
    Check(highStats.splitCount == 0, "standard-inversion: High must not split a face");
    Check(standard.size() == input.size(), "standard-inversion: no face may be added or lost");
    CheckExactReorder(input, standard, "standard-inversion: every face and vertex survives exactly once");

    // The anchors sit far outside the cluster's range, so they must bracket it.
    Check(!standard.empty() && standard.front().ordinal == 1, "standard-inversion: the far anchor comes first");
    Check(!standard.empty() && standard.back().ordinal == 2, "standard-inversion: the near anchor comes last");

    Check(Inversions(standard) > 0, "standard-inversion: a nearer face may precede a farther one");
    Check(Inversions(high) == 0, "standard-inversion: High never inverts depth");

    bool same = standard.size() == high.size();
    for (std::size_t i = 0; same && i < standard.size(); ++i) same = SameTag(standard[i], high[i]);
    Check(!same, "standard-inversion: Standard and High must differ");
}

// ---------------------------------------------------------------------------
// Standard: empty, single-face, zero-span and non-finite inputs stay safe.
// ---------------------------------------------------------------------------
// None of these may crash, invent a face, drop a valid face or leave a
// non-finite vertex in the result.
void TestStandardDegenerateAndNonFiniteInputs() {
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    const double kInf = std::numeric_limits<double>::infinity();

    {  // No input at all.
        std::vector<Triangle> input;
        Statistics stats;
        const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);
        Check(out.empty(), "low-empty: an empty input stays empty");
        Check(stats.inputTriangles == 0 && stats.outputTriangles == 0 && stats.splitCount == 0,
              "low-empty: the statistics stay at zero");
        Check(stats.bucketCount == 0, "low-empty: no bucket is used");
    }
    {  // One face: the depth span is zero and exactly one bucket is filled.
        Triangle one = MakeTriangle(MakeVertex(-0.3, -0.3, 0.4, 0.8),
                                    MakeVertex(0.6, -0.3, 0.4, 0.8),
                                    MakeVertex(-0.3, 0.5, 0.4, 0.8), 4, 40);
        std::vector<Triangle> input{one};
        Statistics stats;
        const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);
        Check(out.size() == 1, "low-single: the one face survives");
        CheckExactReorder(input, out, "low-single: every vertex component is unchanged");
        Check(stats.bucketCount == 1, "low-single: one bucket is used");
    }
    {  // Zero span over several faces: one bucket, so the input order is kept.
        std::vector<Triangle> input;
        for (int i = 0; i < 9; ++i) {
            const double x0 = -0.8 + 0.15 * i;
            Triangle t = MakeTriangle(MakeVertex(x0, -0.5, 0.25, 1.0),
                                      MakeVertex(x0 + 0.8, -0.5, 0.25, 1.0),
                                      MakeVertex(x0, -0.1, 0.25, 1.0),
                                      static_cast<std::uint32_t>(30 + i),
                                      static_cast<std::uint32_t>(900 - 30 * i));
            FillData(t, i);
            input.push_back(t);
        }
        Statistics stats;
        const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);
        Check(out.size() == input.size(), "low-flat: no face may be lost");
        CheckExactReorder(input, out, "low-flat: every face and vertex survives exactly once");
        for (std::size_t i = 0; i < out.size() && i < input.size(); ++i) {
            Check(SameTag(out[i], input[i]), "low-flat: a zero span keeps the input order");
        }
        Check(stats.bucketCount == 1, "low-flat: a zero span fills one bucket");
    }
    {  // Non-finite coordinates and weights are dropped, never ordered on.
        Triangle far = MakeTriangle(MakeVertex(-0.5, -0.5, 1.0, 1.0),
                                    MakeVertex(0.5, -0.5, 0.0, 1.0),
                                    MakeVertex(-0.5, 0.5, 0.0, 1.0), 61, 61);
        Triangle near = MakeTriangle(MakeVertex(-0.4, -0.4, 0.0, 1.0),
                                     MakeVertex(0.4, -0.4, 0.0, 1.0),
                                     MakeVertex(-0.4, 0.4, 0.0, 1.0), 62, 62);
        std::vector<Triangle> input{far, near};
        input.push_back(MakeTriangle(MakeVertex(kNaN, 0.0, 0.5, 1.0),
                                     MakeVertex(0.1, 0.1, 0.5, 1.0),
                                     MakeVertex(0.0, 0.2, 0.5, 1.0), 63, 63));
        input.push_back(MakeTriangle(MakeVertex(kInf, 0.0, 0.5, 1.0),
                                     MakeVertex(0.1, 0.1, 0.5, 1.0),
                                     MakeVertex(0.0, 0.2, 0.5, 1.0), 64, 64));
        input.push_back(MakeTriangle(MakeVertex(0.1, 0.1, 0.5, 0.0),
                                     MakeVertex(0.2, 0.0, 0.5, 1.0),
                                     MakeVertex(0.0, 0.2, 0.5, 1.0), 65, 65));
        input.push_back(MakeTriangle(MakeVertex(0.1, 0.1, 0.5, kInf),
                                     MakeVertex(0.2, 0.0, 0.5, 1.0),
                                     MakeVertex(0.0, 0.2, 0.5, 1.0), 66, 66));
        input.push_back(MakeTriangle(MakeVertex(0.1, -kInf, 0.5, 1.0),
                                     MakeVertex(0.2, 0.0, 0.5, 1.0),
                                     MakeVertex(0.0, 0.2, 0.5, 1.0), 67, 67));
        Statistics stats;
        const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);
        Check(stats.inputTriangles == input.size(), "low-nonfinite: the input count is reported");
        Check(stats.limited, "low-nonfinite: a dropped face must set limited");
        Check(out.size() == 2, "low-nonfinite: only the finite faces survive");
        Check(stats.outputTriangles == out.size(), "low-nonfinite: the output count is reported");
        for (const Triangle& f : out) {
            for (const Vertex& v : f.v) {
                Check(std::isfinite(v.inverseW) && std::isfinite(v.p[0]) &&
                          std::isfinite(v.p[1]) && std::isfinite(v.p[2]),
                      "low-nonfinite: no non-finite vertex may reach the result");
            }
        }
        Check(out.size() == 2 && out[0].ordinal == 61 && out[1].ordinal == 62,
              "low-nonfinite: the surviving faces stay far to near");
    }
    {  // Nothing finite at all: an empty result and no bucket.
        std::vector<Triangle> input;
        for (int i = 0; i < 3; ++i) {
            input.push_back(MakeTriangle(MakeVertex(kNaN, 0.1, 0.5, 1.0),
                                         MakeVertex(0.2, 0.0, 0.5, 1.0),
                                         MakeVertex(0.0, 0.2, 0.5, 1.0),
                                         static_cast<std::uint32_t>(70 + i), 70));
        }
        Statistics stats;
        const std::vector<Triangle> out = Order(input, TransparentSortPrecision::Standard, stats);
        Check(out.empty(), "low-allbad: nothing finite, nothing returned");
        Check(stats.limited, "low-allbad: dropped faces must set limited");
        Check(stats.bucketCount == 0, "low-allbad: no bucket is used");
    }
}

}  // namespace

void TestBatchDraws() {
    using mikumikustudio::transparent_geometry::BatchDraws;
    const auto face = [](double x, double y, double size, std::uint32_t draw, int identity) {
        auto t = MakeTriangle(MakeVertex(x, y, 0.8, 0.7),
                              MakeVertex(x + size, y, 0.2, 0.9),
                              MakeVertex(x, y + size, 0.5, 1.1), draw, 17);
        // Repeated ordinals represent split pieces; payload identifies each.
        for (int v = 0; v < 3; ++v)
            for (int j = 0; j < 24; ++j) t.v[v].data[j] = float(identity * 100 + v * 24 + j);
        return t;
    };
    const auto intact = [](const std::vector<Triangle>& a, const std::vector<Triangle>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].draw != b[i].draw || a[i].ordinal != b[i].ordinal) return false;
            for (int v = 0; v < 3; ++v)
                if (std::memcmp(a[i].v[v].p, b[i].v[v].p, sizeof(a[i].v[v].p)) ||
                    a[i].v[v].inverseW != b[i].v[v].inverseW ||
                    std::memcmp(a[i].v[v].data, b[i].v[v].data, sizeof(a[i].v[v].data))) return false;
        }
        return true;
    };
    const std::vector<Triangle> input{face(-0.8, -0.8, 0.2, UINT32_MAX, 0),
                                      face(0.4, 0.4, 0.2, 9, 1),
                                      face(-0.8, -0.8, 0.2, UINT32_MAX, 2)};
    auto result = input;
    Check(BatchDraws(result, 1280, 720), "batch: disjoint material can move");
    Check(intact(result, {input[0], input[2], input[1]}),
          "batch: sparse draws, duplicate ordinals and all vertex payloads survive");
    result = input;
    Check(!BatchDraws(result, 1280, 720, input.size()) && intact(result, input),
          "batch: exhausted search leaves original sequence intact");
    Check(!BatchDraws(result, 0, 720) && intact(result, input), "batch: invalid viewport is unchanged");
    auto overlapping = input;
    overlapping[1] = face(-0.8, -0.8, 0.2, 9, 1);
    result = overlapping;
    Check(!BatchDraws(result, 1280, 720) && intact(result, overlapping),
          "batch: overlapping A B A must retain all three draws");
    auto touching = input;
    touching[1] = face(-0.6, -0.8, 0.2, 9, 1);
    result = touching;
    Check(!BatchDraws(result, 1280, 720) && intact(result, touching),
          "batch: a common edge cannot cross the original order");
    touching[1] = face(-0.6 + 0.25 * 2 / 1280, -0.8, 0.2, 9, 1);
    result = touching;
    Check(!BatchDraws(result, 1280, 720) && intact(result, touching),
          "batch: subpixel gaps retain rasterization margin");
    auto invalid = input;
    invalid[1].v[0].p[0] = std::numeric_limits<double>::quiet_NaN();
    result = invalid;
    Check(!BatchDraws(result, 1280, 720) && intact(result, invalid),
          "batch: invalid projection cannot be reordered");

    // Independently composite sample points. This also checks overlaps within
    // the same material, which may sample different texture colours.
    std::vector<Triangle> scene;
    for (int row = 0; row < 6; ++row) for (int col = 0; col < 6; ++col)
        for (int layer = 0; layer < 4; ++layer)
            scene.push_back(face(-0.9 + col * 0.3 + layer * 0.005,
                                 -0.9 + row * 0.3, 0.28, layer % 3, int(scene.size()) + 1));
    auto batched = scene;
    Check(BatchDraws(batched, 128, 128), "batch: separated translucent islands reduce draws");
    const auto composite = [](const std::vector<Triangle>& triangles, double x, double y) {
        double colour = 0;
        for (const auto& t : triangles) {
            bool inside = true;
            for (int v = 0; v < 3; ++v) {
                const auto& a = t.v[v].p; const auto& b = t.v[(v + 1) % 3].p;
                inside &= (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) >= 0;
            }
            if (inside) colour = colour * 0.6 + double(t.v[0].data[0]) * 0.4;
        }
        return colour;
    };
    bool samePixels = true;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x)
        samePixels &= composite(scene, (x + 0.5) / 64 - 1, (y + 0.5) / 64 - 1) ==
                      composite(batched, (x + 0.5) / 64 - 1, (y + 0.5) / 64 - 1);
    Check(samePixels, "batch: independent alpha compositing is identical at every sample");
}

void TestLowerStrengths() {
    std::vector<Triangle> input;
    for (unsigned i = 0; i < 64; ++i) {
        const double z = double(i) / 63;
        input.push_back(MakeTriangle(MakeVertex(-0.5, -0.5, z, 1),
                                     MakeVertex(0.5, -0.5, z, 1),
                                     MakeVertex(0, 0.5, z, 1), i % 3, i));
    }
    Statistics lowStats, standardStats, highStats;
    const auto low = Order(input, TransparentSortPrecision::Low, lowStats);
    const auto standard = Order(input, TransparentSortPrecision::Standard, standardStats);
    const auto high = Order(input, TransparentSortPrecision::High, highStats);
    Check(lowStats.bucketCount == 16 && standardStats.bucketCount == 64 && highStats.bucketCount == 0,
          "lower strengths: 16 buckets, 64 buckets, whole-face stable depth sort");
    Check(low[0].ordinal == 60 && low[1].ordinal == 61 &&
          standard[0].ordinal == 63 && standard[1].ordinal == 62 && high[0].ordinal == 63,
          "lower strengths: coarse bucket keeps source order while finer sort separates depths");
    for (const auto* result : {&low, &standard, &high})
        CheckExactReorder(input, *result, "lower strengths preserve every complete face");
    Check(!lowStats.splitCount && !standardStats.splitCount && !highStats.splitCount,
          "lower strengths never split geometry");
}

int main() {
    TestLowerStrengths();
    TestBatchDraws();
    TestIntersectingFaces();
    TestCoplanarStableOrder();
    TestDrawNumbersPreserved();
    TestCyclicOcclusionRing();
    TestPerspectiveInterpolation();
    TestStandardBucketOrderAcrossDraws();
    TestHighDepthOrderAndOrdinalTies();
    TestStandardBucketInversionVersusHigh();
    TestStandardDegenerateAndNonFiniteInputs();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("Transparent geometry regressions passed.");
    return 0;
}
