#include "mikumikustudio/transparent_geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>

namespace mikumikustudio::transparent_geometry {
namespace {
struct Plane { double n[3]{}, d = 0; };
struct Limits { std::size_t extra; unsigned depth; double epsilon; };
Limits Budget(TransparentSortPrecision precision) {
    switch (precision) {
    // Maximum keeps the previous top-tier caps: its extra accuracy comes from
    // the peeling path, and a device or material without peeling still gets a
    // bounded tree here.
    case TransparentSortPrecision::Maximum: return {4000000, 512, 2e-13};
    // Off (transparent sorting disabled) and any unknown value land here too,
    // which keeps an unexpected value bounded instead of unbounded.
    default: return {250000, 128, 1e-11};
    }
}
double Distance(const Plane& p, const Vertex& v) {
    return p.n[0]*v.p[0] + p.n[1]*v.p[1] + p.n[2]*v.p[2] + p.d;
}
bool Valid(const Triangle& t) {
    for (const auto& v : t.v) {
        if (!(v.inverseW > 0) || !std::isfinite(v.inverseW)) return false;
        for (auto p : v.p) if (!std::isfinite(p)) return false;
    }
    return true;
}
bool VisibleArea(const Triangle& t) {
    const auto* a = t.v[0].p; const auto* b = t.v[1].p; const auto* c = t.v[2].p;
    return std::abs((b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])) > 1e-24;
}
bool FacePlane(const Triangle& t, Plane& plane) {
    double a[3], b[3];
    for (int i = 0; i < 3; ++i) { a[i] = t.v[1].p[i]-t.v[0].p[i]; b[i] = t.v[2].p[i]-t.v[0].p[i]; }
    plane.n[0] = a[1]*b[2] - a[2]*b[1];
    plane.n[1] = a[2]*b[0] - a[0]*b[2];
    plane.n[2] = a[0]*b[1] - a[1]*b[0];
    const double length = std::sqrt(plane.n[0]*plane.n[0] + plane.n[1]*plane.n[1] + plane.n[2]*plane.n[2]);
    if (!(length > 1e-30) || !std::isfinite(length)) return false;
    plane.d = 0;
    for (int i = 0; i < 3; ++i) { plane.n[i] /= length; plane.d -= plane.n[i]*t.v[0].p[i]; }
    return true;
}
// -1: negative, 0: coplanar, 1: positive, 2: straddling.
int Classify(const Triangle& t, const Plane& plane, double epsilon) {
    bool positive = false, negative = false;
    for (const auto& v : t.v) {
        const double d = Distance(plane, v);
        positive |= d > epsilon; negative |= d < -epsilon;
    }
    return positive && negative ? 2 : positive ? 1 : negative ? -1 : 0;
}
// Depth key shared by the High order and the two bucket passes: the sum of
// the three projected Z values.  Larger values are farther from the viewer.
double Depth(const Triangle& t) {
    return t.v[0].p[2]+t.v[1].p[2]+t.v[2].p[2];
}
void Approximate(std::vector<Triangle>& triangles, std::size_t first = 0,
                 std::size_t last = std::numeric_limits<std::size_t>::max()) {
    last = std::min(last, triangles.size());
    if (first >= last) return;
    std::stable_sort(triangles.begin() + first, triangles.begin() + last,
                     [](const Triangle& a, const Triangle& b) {
        const double za = Depth(a), zb = Depth(b);
        return za == zb ? a.ordinal < b.ordinal : za > zb;
    });
}
// One stable counting pass over a fixed number of depth buckets, far to near.
// Every triangle lands in exactly one bucket, so the result holds the same
// triangles as the input, only reordered, and the input order is preserved
// inside a bucket.  A zero span collapses to a single bucket and a non-finite
// depth leaves the order untouched; both are safe, not errors.  Returns how
// many buckets the pass filled.
template<std::size_t kBuckets>
std::size_t BucketOrder(std::vector<Triangle>& triangles) {
    if (triangles.empty()) return 0;
    double nearDepth = std::numeric_limits<double>::infinity();
    double farDepth = -std::numeric_limits<double>::infinity();
    for (const auto& t : triangles) {
        const double depth = Depth(t);
        if (!std::isfinite(depth)) return 0;
        nearDepth = std::min(nearDepth, depth);
        farDepth = std::max(farDepth, depth);
    }
    const bool spread = farDepth > nearDepth;
    const double scale = spread ? double(kBuckets)/(farDepth-nearDepth) : 0;
    // Bucket 0 holds the farthest depth, bucket kBuckets-1 the nearest.
    const auto bucketOf = [&](const Triangle& t) {
        if (!spread) return std::size_t{0};
        // Clamp before the conversion: a degenerate span can leave a NaN or an
        // infinite step, and converting either to an integer is undefined.
        const double forward = (Depth(t)-nearDepth)*scale;
        const std::size_t step = forward > 0 ? std::size_t(std::min(forward, double(kBuckets))) : 0;
        return kBuckets - 1 - std::min(kBuckets - 1, step);
    };
    std::array<std::size_t, kBuckets> counts{};
    for (const auto& t : triangles) ++counts[bucketOf(t)];
    std::array<std::size_t, kBuckets> write{};
    std::size_t running = 0, used = 0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
        write[b] = running;
        running += counts[b];
        if (counts[b]) ++used;
    }
    // Writing the input in its own order is what keeps each bucket stable.
    std::vector<Triangle> ordered(triangles.size());
    for (const auto& t : triangles) ordered[write[bucketOf(t)]++] = t;
    triangles.swap(ordered);
    return used;
}
// Original-face linear depth for the bucket tiers. A non-finite caller value
// falls back to the projected Z sum, which is the legacy bucket key.
double PolicyBucketDepth(const Triangle& t) {
    return std::isfinite(t.linearDepth) ? t.linearDepth : Depth(t);
}
// Stable far-to-near bucketing for one policy scope. Bounds come from every
// valid source face, including outside ones; only visible faces enter the
// buckets. A finite far-minus-near span uses the direct ratio, which is exact
// for tiny and ordinary ranges; only a span that overflows to infinity uses
// the centered half-difference form.
template<std::size_t kBuckets>
std::size_t PolicyBucketGroup(std::vector<Triangle>& triangles, std::size_t first,
                              std::size_t last, bool& limited) {
    if (first >= last) return 0;
    double nearDepth = std::numeric_limits<double>::infinity();
    double farDepth = -std::numeric_limits<double>::infinity();
    for (std::size_t i = first; i < last; ++i) {
        const double depth = PolicyBucketDepth(triangles[i]);
        // A face without a finite depth key cannot be placed by comparison.
        if (!std::isfinite(depth)) { limited = true; return 0; }
        nearDepth = std::min(nearDepth, depth);
        farDepth = std::max(farDepth, depth);
    }
    const bool spread = farDepth > nearDepth;
    const double span = farDepth - nearDepth;
    const bool direct = spread && std::isfinite(span);
    const double middle = direct ? 0.0 : nearDepth * 0.5 + farDepth * 0.5;
    const double halfSpan = direct ? 0.0 : farDepth * 0.5 - nearDepth * 0.5;
    const auto bucketOf = [&](const Triangle& t) -> std::size_t {
        if (!spread) return 0;
        double forward = 0;
        if (direct) {
            forward = (PolicyBucketDepth(t) - nearDepth) / span;
        } else {
            const double offset = (PolicyBucketDepth(t) - middle) / halfSpan;
            forward = 0.5 + 0.5 * std::clamp(offset, -1.0, 1.0);
        }
        // Clamp before the conversion so no non-finite or negative value is
        // converted to an integer.
        if (!(forward > 0)) return kBuckets - 1;
        const double scaled = forward * double(kBuckets);
        if (scaled >= double(kBuckets)) return 0;
        const std::size_t step = static_cast<std::size_t>(scaled);
        return kBuckets - 1 - std::min(kBuckets - 1, step);
    };
    std::array<std::size_t, kBuckets> counts{};
    for (std::size_t i = first; i < last; ++i)
        if (!(triangles[i].flags & kOutsideFrustum)) ++counts[bucketOf(triangles[i])];
    std::array<std::size_t, kBuckets> write{};
    std::size_t running = 0, used = 0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
        write[b] = running;
        running += counts[b];
        if (counts[b]) ++used;
    }
    std::vector<Triangle> ordered(running);
    for (std::size_t i = first; i < last; ++i) {
        if (triangles[i].flags & kOutsideFrustum) continue;
        ordered[write[bucketOf(triangles[i])]++] = triangles[i];
    }
    // Refill only the original visible slots, in bucket order. Outside faces
    // keep their slots here; the caller compacts them when the policy asks
    // for that, so no stale visible copy can survive in the tail.
    std::size_t at = 0;
    for (std::size_t i = first; i < last; ++i)
        if (!(triangles[i].flags & kOutsideFrustum)) triangles[i] = std::move(ordered[at++]);
    return used;
}
Plane ChoosePlane(const std::vector<Triangle>& triangles, double epsilon) {
    Plane best{};
    double bestScore = std::numeric_limits<double>::infinity();
    const auto consider = [&](const Plane& candidate) {
        std::size_t positive = 0, negative = 0, crossing = 0, coplanar = 0;
        for (const auto& t : triangles) {
            switch (Classify(t, candidate, epsilon)) {
            case -1: ++negative; break;
            case 1: ++positive; break;
            case 2: ++crossing; break;
            default: ++coplanar; break;
            }
        }
        // A plane with no progress cannot partition this leaf.
        if (!coplanar && (!positive || !negative) && !crossing) return;
        const double score = crossing * 4.0 + std::abs(double(positive)-double(negative)) * .5 - coplanar;
        if (score < bestScore) { bestScore = score; best = candidate; }
    };
    const std::size_t samples = std::min<std::size_t>(12, triangles.size());
    for (std::size_t i = 0; i < samples; ++i) {
        Plane p;
        if (FacePlane(triangles[i*(triangles.size()-1)/std::max<std::size_t>(1, samples-1)], p)) consider(p);
    }
    if (triangles.size() > 64) {
        std::vector<double> centers(triangles.size());
        for (int axis = 0; axis < 3; ++axis) {
            for (std::size_t i = 0; i < triangles.size(); ++i)
                centers[i] = (triangles[i].v[0].p[axis]+triangles[i].v[1].p[axis]+triangles[i].v[2].p[axis])/3;
            auto mid = centers.begin() + centers.size()/2;
            std::nth_element(centers.begin(), mid, centers.end());
            Plane p; p.n[axis] = 1; p.d = -*mid; consider(p);
        }
    }
    return best;
}
Vertex Intersection(const Vertex& a, const Vertex& b, double da, double db) {
    const double t = std::clamp(da/(da-db), 0.0, 1.0);
    Vertex out{};
    out.inverseW = a.inverseW + t*(b.inverseW-a.inverseW);
    const double lambda = t*b.inverseW/out.inverseW;
    for (int i = 0; i < 3; ++i) out.p[i] = a.p[i] + t*(b.p[i]-a.p[i]);
    for (int i = 0; i < 24; ++i) out.data[i] = float(a.data[i] + lambda*(double(b.data[i])-a.data[i]));
    return out;
}
void EmitPolygon(const std::array<Vertex, 5>& polygon, unsigned count, const Triangle& original,
                 std::vector<Triangle>& destination) {
    for (unsigned i = 1; i + 1 < count; ++i) {
        // Copy every field of the source face, then replace only the geometry.
        // Split pieces therefore keep the source object, flags, source indices
        // and linear depth instead of resetting them to defaults.
        Triangle t = original;
        t.v[0] = polygon[0]; t.v[1] = polygon[i]; t.v[2] = polygon[i+1];
        if (VisibleArea(t)) destination.push_back(t);
    }
}
void Split(const Triangle& triangle, const Plane& plane,
           std::vector<Triangle>& positive, std::vector<Triangle>& negative) {
    // Use exact distances here, not the classification tolerance: moving an
    // endpoint to a plane can introduce cracks or erase very thin fragments.
    std::array<Vertex, 5> front{}, back{}; unsigned nf = 0, nb = 0;
    auto previous = triangle.v[2]; double dp = Distance(plane, previous);
    for (const auto& current : triangle.v) {
        const double dc = Distance(plane, current);
        if ((dp < 0 && dc > 0) || (dp > 0 && dc < 0)) {
            auto crossing = Intersection(previous, current, dp, dc);
            front[nf++] = crossing; back[nb++] = crossing;
        }
        if (dc >= 0) front[nf++] = current;
        if (dc <= 0) back[nb++] = current;
        previous = current; dp = dc;
    }
    EmitPolygon(front, nf, triangle, positive);
    EmitPolygon(back, nb, triangle, negative);
}
struct Work { std::vector<Triangle> triangles; unsigned depth = 0; bool emit = false; };
// Shared post-filter BSP core for the legacy Maximum/Off entrance and the
// explicit policy. The caller supplies already filtered triangles and its own
// statistics record; the budget still depends on the requested precision so
// the legacy Off path keeps its historical limits.
std::vector<Triangle> MaximumSplit(std::vector<Triangle> triangles,
                                   TransparentSortPrecision precision, Statistics& stats) {
    const auto limit = Budget(precision);
    std::size_t extra = 0;
    std::vector<Triangle> output;
    output.reserve(triangles.size());
    std::vector<Work> stack;
    stack.push_back({std::move(triangles), 0, false});
    const auto fallback = [&](std::vector<Triangle>& piece) {
        // A non-finite projected key cannot enter a strict weak order.
        for (const auto& t : piece)
            if (!std::isfinite(Depth(t))) { stats.limited = true; return; }
        Approximate(piece);
    };
    while (!stack.empty()) {
        auto work = std::move(stack.back()); stack.pop_back();
        auto& piece = work.triangles;
        if (piece.empty()) continue;
        if (work.emit || piece.size() == 1) {
            output.insert(output.end(), piece.begin(), piece.end()); continue;
        }
        if (work.depth >= limit.depth || extra >= limit.extra) {
            stats.limited = true; fallback(piece);
            output.insert(output.end(), piece.begin(), piece.end()); continue;
        }
        const Plane plane = ChoosePlane(piece, limit.epsilon);
        std::size_t crossings = 0;
        for (const auto& t : piece) crossings += Classify(t, plane, limit.epsilon) == 2;
        if (crossings > (limit.extra - extra) / 2) {
            stats.limited = true; fallback(piece);
            output.insert(output.end(), piece.begin(), piece.end()); continue;
        }
        std::vector<Triangle> positive, negative, coplanar;
        positive.reserve(piece.size() / 2); negative.reserve(piece.size() / 2);
        for (const auto& t : piece) {
            switch (Classify(t, plane, limit.epsilon)) {
            case -1: negative.push_back(t); break;
            case 1: positive.push_back(t); break;
            case 2: Split(t, plane, positive, negative); ++stats.splitCount; break;
            default: coplanar.push_back(t); break;
            }
        }
        const auto generated = positive.size() + negative.size() + coplanar.size();
        if (generated > piece.size()) extra += generated - piece.size();
        if (coplanar.empty() && (positive.empty() || negative.empty())) {
            stats.limited = true; fallback(piece);
            output.insert(output.end(), piece.begin(), piece.end()); continue;
        }
        std::stable_sort(coplanar.begin(), coplanar.end(),
            [](const Triangle& a, const Triangle& b) { return a.ordinal < b.ordinal; });
        // Viewing rays point along +Z in NDC. The stack is LIFO: near, coplanar
        // and far are pushed in that order so the farther child is emitted first.
        if (plane.n[2] >= 0) {
            stack.push_back({std::move(negative), work.depth + 1, false});
            stack.push_back({std::move(coplanar), work.depth + 1, true});
            stack.push_back({std::move(positive), work.depth + 1, false});
        } else {
            stack.push_back({std::move(positive), work.depth + 1, false});
            stack.push_back({std::move(coplanar), work.depth + 1, true});
            stack.push_back({std::move(negative), work.depth + 1, false});
        }
    }
    stats.outputTriangles = output.size();
    return output;
}
} // namespace

unsigned FrustumOutsidePlanes(const double clip[4]) noexcept {
    if (!clip) return 0;
    double scale = 1;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(clip[i])) return 0;
        scale = std::max(scale, std::abs(clip[i]));
    }
    const double epsilon = scale * 1e-9;
    // D3D homogeneous planes: w+x, w-x, w+y, w-y, z, w-z.
    const double distances[] = {clip[3] + clip[0], clip[3] - clip[0],
        clip[3] + clip[1], clip[3] - clip[1], clip[2], clip[3] - clip[2]};
    unsigned mask = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (distances[i] < -epsilon) mask |= 1u << i;
    return mask;
}

std::vector<Triangle> Order(const std::vector<Triangle>& input,
                           TransparentSortPrecision precision, Statistics& stats) {
    stats = {}; stats.inputTriangles = input.size();
    std::vector<Triangle> filtered; filtered.reserve(input.size());
    for (const auto& t : input) {
        if (!Valid(t)) { stats.limited = true; continue; }
        if (VisibleArea(t)) filtered.push_back(t);
    }
    // High now uses the former Standard whole-triangle depth order.
    if (precision == TransparentSortPrecision::High) {
        Approximate(filtered); stats.outputTriangles = filtered.size(); return filtered;
    }
    // Standard inherits the former Low's 64 buckets; Low reduces this to 16.
    // Both keep input order inside a bucket and never split or repeat a face.
    if (precision == TransparentSortPrecision::Low || precision == TransparentSortPrecision::Standard) {
        stats.bucketCount = precision == TransparentSortPrecision::Low
            ? BucketOrder<16>(filtered) : BucketOrder<64>(filtered);
        stats.outputTriangles = filtered.size(); return filtered;
    }
    return MaximumSplit(std::move(filtered), precision, stats);
}
std::vector<Triangle> OrderWithPolicy(const std::vector<Triangle>& input,
    TransparentSortPrecision precision, Statistics& stats, const OrderPolicy& policy) {
    Statistics local;
    local.inputTriangles = input.size();
    if (input.empty()) { stats = {}; return {}; }
    // Publish only after the whole result exists, so a failed allocation keeps
    // the caller's statistics untouched.
    const auto publish = [&](std::vector<Triangle> result) {
        local.outputTriangles = result.size();
        stats = local;
        return result;
    };
    // Off is the raw pass-through: invalid, degenerate and outside faces all
    // stay exactly as submitted.
    if (precision == TransparentSortPrecision::Off) return publish(input);
    switch (precision) {
    case TransparentSortPrecision::Low:
    case TransparentSortPrecision::Standard:
    case TransparentSortPrecision::High:
    case TransparentSortPrecision::Maximum: break;
    default:
        // An unknown value is not a request to run a bounded BSP pass.
        local.limited = true;
        return publish(input);
    }
    // Scope boundaries come from the authored input before validity filtering,
    // so an invalid face of another object cannot merge two runs of one object.
    const auto segmentEnd = [&](std::size_t first) {
        std::size_t end = first + 1;
        while (end < input.size() && input[end].object == input[first].object) ++end;
        return end;
    };
    const auto takeScope = [&](std::size_t first, std::size_t end) {
        std::vector<Triangle> scope;
        scope.reserve(end - first);
        for (std::size_t i = first; i < end; ++i) {
            const Triangle& t = input[i];
            if (!Valid(t)) { local.limited = true; continue; }
            if (VisibleArea(t)) scope.push_back(t);
        }
        return scope;
    };
    // Apply one scope processor to every scope and keep authored group order.
    const auto runScopes = [&](const auto& process) {
        std::vector<Triangle> output;
        if (policy.crossObjects) {
            output = process(takeScope(0, input.size()));
        } else {
            for (std::size_t first = 0; first < input.size();) {
                const std::size_t end = segmentEnd(first);
                auto part = process(takeScope(first, end));
                output.insert(output.end(), part.begin(), part.end());
                first = end;
            }
        }
        return output;
    };
    const auto countOutside = [](const std::vector<Triangle>& scope) {
        return static_cast<std::size_t>(std::count_if(scope.begin(), scope.end(),
            [](const Triangle& t) { return (t.flags & kOutsideFrustum) != 0; }));
    };
    if (precision == TransparentSortPrecision::Low ||
        precision == TransparentSortPrecision::Standard ||
        precision == TransparentSortPrecision::High) {
        const auto bucketProcess = [&](std::vector<Triangle> scope) {
            local.offscreenTriangles += countOutside(scope);
            std::size_t used = 0;
            if (precision == TransparentSortPrecision::Low)
                used = PolicyBucketGroup<16>(scope, 0, scope.size(), local.limited);
            else if (precision == TransparentSortPrecision::Standard)
                used = PolicyBucketGroup<64>(scope, 0, scope.size(), local.limited);
            else
                used = PolicyBucketGroup<256>(scope, 0, scope.size(), local.limited);
            local.bucketCount = std::max(local.bucketCount, used);
            if (!policy.retainOutside)
                scope.erase(std::remove_if(scope.begin(), scope.end(), [](const Triangle& t) {
                    return (t.flags & kOutsideFrustum) != 0;
                }), scope.end());
            return scope;
        };
        return publish(runScopes(bucketProcess));
    }
    // Whole-face Maximum accepts the caller's unclipped source faces: outside
    // flags and retainOutside are irrelevant, no vertex is generated and the
    // source indices pass through untouched.
    const auto wholeFaceProcess = [&](std::vector<Triangle> scope) {
        for (const auto& t : scope) {
            if (!std::isfinite(Depth(t))) {
                // Keep the faces but do not compare an invalid key.
                local.limited = true;
                return scope;
            }
        }
        Approximate(scope, 0, scope.size());
        return scope;
    };
    if (!policy.splitIntersections) return publish(runScopes(wholeFaceProcess));
    const auto splitProcess = [&](std::vector<Triangle> scope) {
        // BSP plane and edge arithmetic is only safe for moderately scaled
        // projected coordinates. An extreme finite magnitude can overflow into
        // non-finite planes, crossings and then generated vertices, so order
        // whole faces instead of feeding those into any later comparison.
        for (const auto& t : scope) {
            for (const auto& v : t.v) {
                if (!(std::abs(v.p[0]) <= 1e150) || !(std::abs(v.p[1]) <= 1e150) ||
                    !(std::abs(v.p[2]) <= 1e150)) {
                    local.limited = true;
                    return wholeFaceProcess(std::move(scope));
                }
            }
        }
        Statistics part;
        auto ordered = MaximumSplit(std::move(scope), TransparentSortPrecision::Maximum, part);
        local.splitCount += part.splitCount;
        local.limited = local.limited || part.limited;
        return ordered;
    };
    return publish(runScopes(splitProcess));
}
bool BatchDraws(std::vector<Triangle>& ordered, unsigned width, unsigned height,
                std::size_t budget) {
    const std::size_t n = ordered.size();
    if (n < 2 || !width || !height || n > budget) return false;
    const auto runs = [](const auto& sequence) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < sequence.size(); ++i)
            count += i == 0 || sequence[i].draw != sequence[i - 1].draw;
        return count;
    };
    const auto originalRuns = runs(ordered);
    if (originalRuns < 3) return false;

    // Each screen tile carries a chain in original order. Sharing a tile can
    // add an unnecessary dependency, but never removes a visible dependency.
    // This bounds work by covered tiles rather than overlapping triangle pairs
    // (thousands of small overlapping faces made pairwise analysis expensive).
    constexpr unsigned grid = 256;
    struct Bounds { unsigned x0, x1, y0, y1; };
    std::vector<Bounds> bounds;
    bounds.reserve(n);
    std::vector<std::uint32_t> owners;
    owners.reserve(n);
    const auto cell = [](double p) {
        return static_cast<unsigned>(std::clamp((p + 1) * (grid / 2), 0.0, double(grid - 1)));
    };
    for (std::size_t i = 0; i < n; ++i) {
        double minX = 1, maxX = -1, minY = 1, maxY = -1;
        for (int v = 0; v < 3; ++v) {
            const auto& p = ordered[i].v[v].p;
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                !std::isfinite(p[2])) return false;
            minX = std::min(minX, p[0]); maxX = std::max(maxX, p[0]);
            minY = std::min(minY, p[1]); maxY = std::max(maxY, p[1]);
        }
        // One pixel on each side includes multisample and rounding margins.
        Bounds b{cell(minX - 2.0 / width), cell(maxX + 2.0 / width),
                 cell(minY - 2.0 / height), cell(maxY + 2.0 / height)};
        const std::size_t visits = std::size_t(b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1);
        if (visits > budget) return false;
        budget -= visits;
        bounds.push_back(b);
        owners.push_back(ordered[i].draw);
    }
    // Draw identities may be sparse UINTs. Never size an array by the largest.
    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    if (owners.size() < 2) return false;
    std::vector<std::size_t> owner(n);
    for (std::size_t i = 0; i < n; ++i)
        owner[i] = std::lower_bound(owners.begin(), owners.end(), ordered[i].draw) - owners.begin();

    std::vector<std::size_t> degree(n, 0);
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    std::vector<std::size_t> last(grid * grid, n), seen(n, n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = bounds[i];
        for (unsigned y = b.y0; y <= b.y1; ++y) for (unsigned x = b.x0; x <= b.x1; ++x) {
            auto& previous = last[y * grid + x];
            if (previous != n && seen[previous] != i) {
                seen[previous] = i;
                edges.push_back({previous, i});
                ++degree[i];
            }
            previous = i;
        }
    }

    // Edges follow sequence positions, not source ordinals (split pieces can
    // share an ordinal). The resulting graph is acyclic by construction.
    std::vector<std::size_t> offsets(n + 1, 0);
    for (const auto& edge : edges) ++offsets[edge.first + 1];
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    auto write = offsets;
    std::vector<std::size_t> targets(edges.size());
    for (const auto& edge : edges) targets[write[edge.first]++] = edge.second;
    using Queue = std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>>;
    std::vector<Queue> ready(owners.size());
    Queue earliest;
    const auto enqueue = [&](std::size_t i) { ready[owner[i]].push(i); earliest.push(i); };
    for (std::size_t i = 0; i < n; ++i) if (!degree[i]) enqueue(i);
    std::vector<bool> emitted(n, false);
    std::vector<std::size_t> permutation;
    permutation.reserve(n);
    std::size_t current = owners.size(), outputRuns = 0;
    while (permutation.size() < n) {
        if (current == owners.size() || ready[current].empty()) {
            while (!earliest.empty() && emitted[earliest.top()]) earliest.pop();
            if (earliest.empty()) return false;
            current = owner[earliest.top()];
            if (++outputRuns >= originalRuns) return false;
        }
        const auto i = ready[current].top(); ready[current].pop();
        emitted[i] = true;
        permutation.push_back(i);
        for (auto at = offsets[i]; at < offsets[i + 1]; ++at)
            if (--degree[targets[at]] == 0) enqueue(targets[at]);
    }
    std::vector<Triangle> result;
    result.reserve(n);
    for (auto i : permutation) result.push_back(ordered[i]);
    ordered.swap(result);
    return true;
}
} // namespace mikumikustudio::transparent_geometry
