#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <functional>
#include <utility>
#include <vector>

namespace mikumikustudio {

// Projected coordinates: z is device depth in [0, 1], increasing away from the eye.
struct AlphaScreenVertex { float x, y, z; };
struct AlphaScreenTriangle { AlphaScreenVertex p[3]; };
struct AlphaMaterialGeometry { std::vector<AlphaScreenTriangle> triangles; };
enum class AlphaOrderResult { Unchanged, Reordered, Unsupported };
struct AlphaOrderDiagnostic {
    const char* reason = "none";
    std::size_t first = 0, second = 0;
};

namespace alpha_order_detail {
struct Point { double x, y; };
inline double Cross(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
struct Triangle {
    Point p[3];
    double z[3];
    double area;
    double minX, maxX, minY, maxY;
    std::size_t material;
};

// Outline test for two convex triangles in 2-D. For each directed edge of A,
// then of B, sign scales the cross product of that edge with each vertex of the
// other triangle; sign is the triangle's own area sign, so pmx double-sided
// data works either way and positive means the vertex is on the inner side.
// All three strictly outside one edge, past that edge's rounding margin, is a
// separation; every touch or unsettled case counts as overlapping.
inline bool TrianglesOverlap2D(const Triangle& a, const Triangle& b, double padding = 0) {
    constexpr double kUlps = 4.0;
    const Point* shapes[2] = {a.p, b.p};
    const double signs[2] = {a.area > 0 ? 1.0 : -1.0, b.area > 0 ? 1.0 : -1.0};
    for (int shape = 0; shape < 2; ++shape) {
        const Point* own = shapes[shape];
        const Point* other = shapes[1 - shape];
        const double sign = signs[shape];
        for (int edge = 0; edge < 3; ++edge) {
            const Point from = own[edge], to = own[(edge + 1) % 3];
            const double dx = to.x - from.x, dy = to.y - from.y;
            double cross[3];
            double margin = 0;
            for (int v = 0; v < 3; ++v) {
                const double ex = other[v].x - from.x, ey = other[v].y - from.y;
                cross[v] = sign * (dx * ey - dy * ex);
                margin = std::max(margin, kUlps * DBL_EPSILON *
                                              (std::abs(dx * ey) + std::abs(dy * ex)));
            }
            // Both triangles may move by padding along each screen axis.
            margin += 2 * padding * (std::abs(dx) + std::abs(dy));
            if (cross[0] < -margin && cross[1] < -margin && cross[2] < -margin)
                return false;
        }
    }
    return true;
}

// Intersection of two convex triangles. Keeping either winding is essential for
// PMX double-sided materials. Buffers also allow repeated boundary vertices.
inline std::size_t Intersect(const Triangle& a, const Triangle& b, Point* out) {
    Point current[16], next[16];
    std::copy(a.p, a.p + 3, current);
    std::size_t count = 3;
    const double winding = b.area > 0 ? 1.0 : -1.0;
    for (int edge = 0; edge < 3 && count; ++edge) {
        std::size_t written = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Point from = current[i], to = current[(i + 1) % count];
            const double f = winding * Cross(b.p[edge], b.p[(edge + 1) % 3], from);
            const double t = winding * Cross(b.p[edge], b.p[(edge + 1) % 3], to);
            if (f >= 0) {
                if (written == 16) return 0;
                next[written++] = from;
            }
            if ((f >= 0) != (t >= 0)) {
                if (written == 16) return 0;
                const double u = f / (f - t);
                next[written++] = {from.x + (to.x - from.x) * u,
                                   from.y + (to.y - from.y) * u};
            }
        }
        count = written;
        std::copy(next, next + count, current);
    }
    std::copy(current, current + count, out);
    return count;
}
inline double Depth(const Triangle& t, Point p) {
    const double w1 = Cross(t.p[0], p, t.p[2]) / t.area;
    const double w2 = Cross(t.p[0], t.p[1], p) / t.area;
    return t.z[0] + w1 * (t.z[1] - t.z[0]) + w2 * (t.z[2] - t.z[0]);
}
} // namespace alpha_order_detail

// Resolve whole-material ordering only when all overlapping triangles agree.
// `order` is an input permutation; its priority breaks topological ties and
// preserves coplanar author order. Crossings, cycles, invalid data and exhausted
// work budgets leave it untouched. This is not per-pixel order-independent alpha.
inline AlphaOrderResult OrderAlphaMaterials(
    const std::vector<AlphaMaterialGeometry>& materials,
    std::vector<std::size_t>& order, std::size_t budget = 262144,
    AlphaOrderDiagnostic* diagnostic = nullptr) {
    if (diagnostic) *diagnostic = {};
    const auto unsupported = [&](const char* reason, std::size_t a = 0, std::size_t b = 0) {
        if (diagnostic) *diagnostic = {reason, a, b};
        return AlphaOrderResult::Unsupported;
    };
    using namespace alpha_order_detail;
    const std::size_t n = materials.size();
    if (order.size() != n || n > 64) return unsupported("invalid");
    std::vector<std::size_t> rank(n, n);
    for (std::size_t i = 0; i < n; ++i) {
        if (order[i] >= n || rank[order[i]] != n) return unsupported("invalid");
        rank[order[i]] = i;
    }
    std::vector<Triangle> triangles;
    for (std::size_t m = 0; m < n; ++m) {
        for (const auto& source : materials[m].triangles) {
            Triangle t{};
            t.material = m;
            for (int v = 0; v < 3; ++v) {
                const auto& p = source.p[v];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                    !std::isfinite(p.z) || p.z < 0 || p.z > 1)
                    return unsupported("invalid");
                t.p[v] = {p.x, p.y};
                t.z[v] = p.z;
            }
            t.area = Cross(t.p[0], t.p[1], t.p[2]);
            if (t.area == 0) return unsupported("invalid");
            t.minX = std::min({t.p[0].x, t.p[1].x, t.p[2].x});
            t.maxX = std::max({t.p[0].x, t.p[1].x, t.p[2].x});
            t.minY = std::min({t.p[0].y, t.p[1].y, t.p[2].y});
            t.maxY = std::max({t.p[0].y, t.p[1].y, t.p[2].y});
            triangles.push_back(t);
        }
    }
    if (n < 2 || triangles.empty()) return AlphaOrderResult::Unchanged;
    std::sort(triangles.begin(), triangles.end(), [](const Triangle& a, const Triangle& b) {
        return a.minX < b.minX;
    });
    std::vector<double> prefixMax;
    prefixMax.reserve(triangles.size());
    for (const auto& t : triangles)
        prefixMax.push_back(prefixMax.empty() ? t.maxX : std::max(prefixMax.back(), t.maxX));

    std::vector<std::uint8_t> edges(n * n, 0);
    constexpr double epsilon = 1e-7; // float device-depth precision
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const auto& a = triangles[i];
        const auto first = std::upper_bound(prefixMax.begin(), prefixMax.begin() + i, a.minX);
        for (std::size_t j = static_cast<std::size_t>(first - prefixMax.begin()); j < i; ++j) {
            // Charge even rejected candidates so long bounding boxes cannot
            // silently turn the broad phase into unbounded quadratic work.
            if (budget == 0) return unsupported("budget");
            --budget;
            const auto& b = triangles[j];
            if (a.material == b.material || b.maxX <= a.minX ||
                a.minY >= b.maxY || b.minY >= a.maxY) continue;
            Point polygon[16];
            const auto count = Intersect(a, b, polygon);
            if (count < 3) continue;
            double area = 0;
            for (std::size_t v = 1; v + 1 < count; ++v)
                area += Cross(polygon[0], polygon[v], polygon[v + 1]);
            if (std::abs(area) <= 1e-12 * std::min(std::abs(a.area), std::abs(b.area))) continue;
            double low = 2, high = -2;
            for (std::size_t v = 0; v < count; ++v) {
                const double d = Depth(a, polygon[v]) - Depth(b, polygon[v]);
                if (!std::isfinite(d)) return unsupported("invalid");
                low = std::min(low, d);
                high = std::max(high, d);
            }
            if (low < -epsilon && high > epsilon) return unsupported("crossing", a.material, b.material);
            bool aFirst;
            if (high > epsilon) aFirst = true;
            else if (low < -epsilon) aFirst = false;
            else aFirst = rank[a.material] < rank[b.material];
            const auto from = aFirst ? a.material : b.material;
            const auto to = aFirst ? b.material : a.material;
            if (edges[to * n + from]) return unsupported("conflict", from, to);
            edges[from * n + to] = 1;
        }
    }
    std::vector<std::size_t> degree(n, 0), result;
    std::vector<bool> emitted(n, false);
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = 0; b < n; ++b) degree[b] += edges[a * n + b];
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t chosen = n;
        for (auto candidate : order) {
            if (!emitted[candidate] && degree[candidate] == 0) { chosen = candidate; break; }
        }
        if (chosen == n) return unsupported("cycle");
        emitted[chosen] = true;
        result.push_back(chosen);
        for (std::size_t b = 0; b < n; ++b) degree[b] -= edges[chosen * n + b];
    }
    if (result == order) return AlphaOrderResult::Unchanged;
    order.swap(result);
    return AlphaOrderResult::Reordered;
}

struct AlphaTriangleReference { std::size_t material, triangle; };

// Candidate pairs cover every vertex position within padding of these anchors.
// Anchors stay fixed on a cache hit; depth and edge directions are never cached.
struct AlphaBatchOverlapCache {
    std::vector<AlphaScreenTriangle> anchors;
    std::vector<std::size_t> materialCounts;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    double padding = 0;
    bool valid = false;
};

// Start with stable back-to-front triangle order, then combine draws only by
// moving triangles that do not overlap on screen. Edges always follow the
// initial order, so even crossing geometry cannot create a graph cycle here.
// Centroid ordering of intersecting triangles is still an approximation.
inline bool BatchAlphaTriangles(const std::vector<AlphaMaterialGeometry>& materials,
                                std::vector<AlphaTriangleReference>& output,
                                std::size_t budget = 262144,
                                AlphaBatchOverlapCache* cache = nullptr) {
    using namespace alpha_order_detail;
    struct Item { Triangle geometry; AlphaTriangleReference source; double depth; std::size_t sourceId; };
    std::vector<Item> items;
    for (std::size_t m = 0; m < materials.size(); ++m) {
        for (std::size_t t = 0; t < materials[m].triangles.size(); ++t) {
            const auto& p = materials[m].triangles[t].p;
            Item item{};
            item.source = {m,t};
            item.sourceId = items.size();
            auto& g = item.geometry;
            for (int v = 0; v < 3; ++v) {
                if (!std::isfinite(p[v].x) || !std::isfinite(p[v].y) ||
                    !std::isfinite(p[v].z) || p[v].z < 0 || p[v].z > 1) return false;
                g.p[v] = {p[v].x,p[v].y};
                item.depth += double(p[v].z) / 3;
            }
            g.area = Cross(g.p[0],g.p[1],g.p[2]);
            if (g.area == 0) return false;
            g.minX = std::min({g.p[0].x,g.p[1].x,g.p[2].x});
            g.maxX = std::max({g.p[0].x,g.p[1].x,g.p[2].x});
            g.minY = std::min({g.p[0].y,g.p[1].y,g.p[2].y});
            g.maxY = std::max({g.p[0].y,g.p[1].y,g.p[2].y});
            items.push_back(item);
        }
    }
    bool reuse = cache && cache->valid && cache->anchors.size() == items.size() &&
        cache->materialCounts.size() == materials.size() &&
        std::isfinite(cache->padding) && cache->padding > 0;
    for (std::size_t m = 0; reuse && m < materials.size(); ++m)
        reuse = cache->materialCounts[m] == materials[m].triangles.size();
    for (std::size_t i = 0; reuse && i < items.size(); ++i)
        for (int v = 0; reuse && v < 3; ++v)
            reuse = std::abs(items[i].geometry.p[v].x - cache->anchors[i].p[v].x) <= cache->padding &&
                    std::abs(items[i].geometry.p[v].y - cache->anchors[i].p[v].y) <= cache->padding;
    double padding = reuse ? cache->padding : 0;
    if (cache && !reuse && !items.empty()) {
        double minX = items[0].geometry.minX, maxX = items[0].geometry.maxX;
        double minY = items[0].geometry.minY, maxY = items[0].geometry.maxY;
        for (const auto& item : items) {
            minX = std::min(minX,item.geometry.minX); maxX = std::max(maxX,item.geometry.maxX);
            minY = std::min(minY,item.geometry.minY); maxY = std::max(maxY,item.geometry.maxY);
        }
        padding = std::max(maxX-minX,maxY-minY) * 0.002;
        if (!std::isfinite(padding) || padding <= 0) return false;
    }
    std::stable_sort(items.begin(),items.end(),[](const Item& a,const Item& b) {
        return a.depth > b.depth;
    });
    const auto n = items.size(), m = materials.size();
    std::vector<std::size_t> degree(n,0);
    std::vector<std::pair<std::size_t,std::size_t>> edges;
    const auto add = [&](std::size_t a,std::size_t b) { edges.push_back({a,b}); ++degree[b]; };
    std::vector<std::pair<std::size_t,std::size_t>> candidatePairs;
    if (reuse) {
        if (cache->pairs.size() > budget) return false;
        std::vector<std::size_t> rankOf(n);
        for (std::size_t rank = 0; rank < n; ++rank) rankOf[items[rank].sourceId] = rank;
        for (const auto& pair : cache->pairs) {
            if (pair.first >= n || pair.second >= n || pair.first == pair.second) return false;
            const auto a = rankOf[pair.first], b = rankOf[pair.second];
            add(std::min(a,b),std::max(a,b));
        }
    } else {
    // A 2-D BVH keeps long facial triangles from turning the broad phase into
    // a quadratic sweep. Intra-material overlaps also need ordering edges:
    // textures may give two triangles of the same material different colours.
    struct Node {
        double minX, maxX, minY, maxY;
        std::size_t begin, end, minRank, left = 0, right = 0;
    };
    std::vector<std::size_t> spatial(n);
    std::iota(spatial.begin(),spatial.end(),0);
    std::vector<Node> nodes;
    nodes.reserve(2*n);
    const auto build = [&](auto&& self, std::size_t begin, std::size_t end) -> std::size_t {
        const auto& seed = items[spatial[begin]].geometry;
        Node node{seed.minX,seed.maxX,seed.minY,seed.maxY,begin,end,n};
        for (auto at = begin; at < end; ++at) {
            const auto rank = spatial[at];
            const auto& g = items[rank].geometry;
            node.minX = std::min(node.minX,g.minX); node.maxX = std::max(node.maxX,g.maxX);
            node.minY = std::min(node.minY,g.minY); node.maxY = std::max(node.maxY,g.maxY);
            node.minRank = std::min(node.minRank,rank);
        }
        const auto index = nodes.size();
        nodes.push_back(node);
        if (end-begin > 8) {
            const auto middle = begin+(end-begin)/2;
            const bool splitX = node.maxX-node.minX >= node.maxY-node.minY;
            std::nth_element(spatial.begin()+begin,spatial.begin()+middle,spatial.begin()+end,
                [&](std::size_t a,std::size_t b) {
                    const auto& x=items[a].geometry; const auto& y=items[b].geometry;
                    return splitX ? x.minX+x.maxX < y.minX+y.maxX : x.minY+x.maxY < y.minY+y.maxY;
                });
            nodes[index].left = self(self,begin,middle);
            nodes[index].right = self(self,middle,end);
        }
        return index;
    };
    if (n) build(build,0,n);
    const auto query = [&](auto&& self,std::size_t ai,std::size_t nodeIndex) -> bool {
        if (!budget) return false;
        --budget;
        const auto& node = nodes[nodeIndex];
        const auto& a = items[ai].geometry;
        if (node.minRank >= ai ||
            node.maxX + 2*padding <= a.minX || a.maxX + 2*padding <= node.minX ||
            node.maxY + 2*padding <= a.minY || a.maxY + 2*padding <= node.minY) return true;
        if (node.left) return self(self,ai,node.left) && self(self,ai,node.right);
        for (auto at = node.begin; at < node.end; ++at) {
            if (!budget) return false;
            --budget;
            const auto bi = spatial[at];
            if (bi >= ai) continue;
            const auto& b = items[bi].geometry;
            if (b.maxX + 2*padding <= a.minX || a.maxX + 2*padding <= b.minX ||
                b.maxY + 2*padding <= a.minY || a.maxY + 2*padding <= b.minY) continue;
            if (!TrianglesOverlap2D(a,b,padding)) continue;
            add(bi,ai);
            if (cache) candidatePairs.push_back({items[bi].sourceId,items[ai].sourceId});
        }
        return true;
    };
    for (std::size_t i = 0; i < n; ++i) if (!query(query,i,0)) return false;
    }
    // Compact adjacency storage avoids one heap allocation per triangle.
    std::vector<std::size_t> offsets(n+1,0);
    for (const auto& edge : edges) ++offsets[edge.first+1];
    std::partial_sum(offsets.begin(),offsets.end(),offsets.begin());
    auto write = offsets;
    std::vector<std::size_t> targets(edges.size());
    for (const auto& edge : edges) targets[write[edge.first]++] = edge.second;
    std::vector<AlphaTriangleReference> result;
    result.reserve(n);
    std::vector<std::vector<std::size_t>> ready(m);
    const auto enqueue = [&](std::size_t i) {
        auto& queue = ready[items[i].source.material];
        queue.push_back(i);
        std::push_heap(queue.begin(),queue.end(),std::greater<std::size_t>{});
    };
    for (std::size_t i = 0; i < n; ++i) if (degree[i] == 0) enqueue(i);
    std::size_t current = m;
    while (result.size() < n) {
        if (current == m || ready[current].empty()) {
            std::size_t choice = n;
            current = m;
            for (std::size_t owner = 0; owner < m; ++owner)
                if (!ready[owner].empty() && ready[owner].front() < choice) {
                    choice = ready[owner].front(); current = owner;
                }
            if (current == m) return false;
        }
        auto& queue = ready[current];
        std::pop_heap(queue.begin(),queue.end(),std::greater<std::size_t>{});
        const auto i = queue.back();
        queue.pop_back();
        result.push_back(items[i].source);
        for (auto at = offsets[i]; at < offsets[i+1]; ++at)
            if (--degree[targets[at]] == 0) enqueue(targets[at]);
    }
    if (cache && !reuse) {
        AlphaBatchOverlapCache next;
        next.anchors.resize(n);
        for (const auto& item : items)
            for (int v = 0; v < 3; ++v)
                next.anchors[item.sourceId].p[v] = {
                    static_cast<float>(item.geometry.p[v].x),
                    static_cast<float>(item.geometry.p[v].y), 0};
        for (const auto& material : materials) next.materialCounts.push_back(material.triangles.size());
        next.pairs = std::move(candidatePairs);
        next.padding = padding;
        next.valid = true;
        *cache = std::move(next);
    }
    output.swap(result);
    return true;
}
} // namespace mikumikustudio
