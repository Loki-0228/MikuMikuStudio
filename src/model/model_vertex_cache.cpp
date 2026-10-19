#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/geometry_workers.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mikumikustudio::model_vertex_cache {
namespace {
constexpr std::size_t perModelLimit = 256u * 1024u * 1024u;
constexpr std::size_t totalLimit = 512u * 1024u * 1024u;
struct Range { const void* address; std::size_t size; };
struct Plan {
    std::vector<unsigned char> key, outlineKey;
    std::vector<Range> ranges;
    std::size_t bytes = 0;
    bool allowed = true;
    bool hostOwned = false;
    void Value(const void* source, std::size_t size) {
        const auto* first = static_cast<const unsigned char*>(source);
        key.insert(key.end(), first, first + size);
    }
    template<class T> void Value(const T& value) { Value(&value, sizeof(value)); }
    template<class T> void Outline(const T& value) {
        const auto* first = reinterpret_cast<const unsigned char*>(&value);
        outlineKey.insert(outlineKey.end(), first, first + sizeof(value));
    }
    template<class T> void Table(const T* address, std::int64_t count) {
        Value(address); Value(count);
        if (hostOwned || !address || count <= 0) return;
        if (std::uint64_t(count) > (perModelLimit - bytes) / sizeof(T)) {
            allowed = false; return;
        }
        const auto size = std::size_t(count) * sizeof(T);
        ranges.push_back({address, size}); bytes += size;
    }
};
struct SavedRange {
    const void* address = nullptr;
    std::vector<unsigned char> bytes;
};
struct Entry {
    std::vector<unsigned char> key, outlineKey;
    std::vector<OutlineVertex> cpuBody;
    std::vector<SavedRange> ranges;
    std::size_t bytes = 0;
    unsigned stable = 0;
    bool valid = false;
    bool wantsCpuBody = false;
};
std::unordered_map<unsigned char*, Entry> entries;
std::unordered_set<unsigned char*> loadedModels;
Stats stats;
bool enabled = true;
bool outlineEnabled = true;

bool Describe(MMDApp* app, unsigned char* model, float distance, Plan& plan) {
    const auto& m = *mdl::Mdl(model);
    const bool pmx = m.physicsMode == 2;
    if (!m.loadComplete || !m.vertexCount || !m.boneTable || !m.boneCount ||
        !m.vertexBuffer || !m.vertexBuffer2 ||
        (pmx && (!m.pmxVertices || !m.materials || m.pmxAdditionalUvCount > 4)) ||
        (!pmx && !m.rawVertices)) return false;
    plan.hostOwned = loadedModels.count(model) != 0;
    plan.key.reserve(4096); plan.ranges.reserve(32);
    plan.Value(plan.hostOwned);
    plan.Value(app); plan.Value(m.vertexBuffer); plan.Value(m.vertexBuffer2);
    plan.Value(m.physicsMode); plan.Value(m.pmxAdditionalUvCount);
    plan.Outline(distance); plan.Outline(app->state.selfShadowEnabled);
    plan.Value(m.boneTable); plan.Value(m.boneCount);
    for (unsigned i = 0; i < m.boneCount; ++i)
        plan.Value(mdl::Bones(model)[i].matInit, sizeof(mdl::Bones(model)[i].matInit));
    if (pmx) {
        plan.Value(m.materials); plan.Value(m.materialCount);
        for (unsigned i = 0; i < m.materialCount; ++i) {
            const auto& material = mdl::Materials(model)[i];
            plan.Outline(material.edgeColor);
            plan.Outline(material.edgeSize);
        }
    }
    plan.Value(m.morphs); plan.Value(m.morphCount);
    if (m.morphs) for (unsigned i = 0; i < m.morphCount; ++i) {
        const auto& morph = mdl::Morphs(model)[i];
        plan.Value(morph.value); plan.Value(morph.type);
        if ((!pmx && i != 0) || (pmx && morph.type == 1))
            plan.Table(morph.vertexEntries, morph.offsetCount);
        if (pmx && morph.type == 0) plan.Table(morph.groupEntries, morph.groupCount);
        if (pmx && morph.type >= 3 && morph.type <= 7) {
            const auto family = morph.type - 3;
            plan.Table(morph.uvEntries[family], morph.uvCounts[family]);
        }
    }
    plan.Table(mdl::BaseVertexMorphTable(model), mdl::BaseVertexMorphCount(model));
    if (pmx) {
        for (unsigned i = 0; i < 5; ++i)
            plan.Table(mdl::UvMorphTables(model).byFamily[i], mdl::UvMorphCounts(model).byFamily[i]);
        plan.Table(m.pmxVertices, m.vertexCount);
    } else plan.Table(m.rawVertices, m.vertexCount);
    return plan.allowed;
}

bool Same(const Entry& entry, const Plan& plan) {
    if (entry.ranges.size() != plan.ranges.size()) return false;
    struct Chunk { const void* before; const void* now; std::size_t size; };
    std::vector<Chunk> chunks;
    constexpr std::size_t chunkSize = 1024u * 1024u;
    chunks.reserve(plan.ranges.size() + plan.bytes / chunkSize);
    for (std::size_t i = 0; i < plan.ranges.size(); ++i) {
        const auto& old = entry.ranges[i]; const auto& now = plan.ranges[i];
        if (old.address != now.address || old.bytes.size() != now.size) return false;
        for (std::size_t offset = 0; offset < now.size; offset += chunkSize)
            chunks.push_back({old.bytes.data() + offset,
                static_cast<const unsigned char*>(now.address) + offset,
                std::min(chunkSize, now.size - offset)});
    }
    int same = 1;
#pragma omp parallel for schedule(static) reduction(&:same) if(plan.bytes >= 4u * 1024u * 1024u) num_threads(GeometryWorkerCount())
    for (std::int64_t i = 0; i < std::int64_t(chunks.size()); ++i)
        same &= std::memcmp(chunks[i].before, chunks[i].now, chunks[i].size) == 0;
    return same != 0;
}
void DropCpuBody(Entry& entry) {
    stats.cpuBytes -= entry.cpuBody.size() * sizeof(OutlineVertex);
    std::vector<OutlineVertex>().swap(entry.cpuBody);
}
}

void Forget(unsigned char* model) noexcept {
    const auto found = entries.find(model);
    if (found != entries.end()) {
        DropCpuBody(found->second);
        stats.bytes -= found->second.bytes; entries.erase(found);
    }
    loadedModels.erase(model);
}
void RegisterLoaded(unsigned char* model) noexcept {
    Forget(model);
    try { loadedModels.insert(model); } catch (...) {}
}
void Invalidate(unsigned char* model) noexcept {
    const auto found = entries.find(model);
    if (found != entries.end()) { found->second.valid = false; DropCpuBody(found->second); }
}
void Clear() noexcept { entries.clear(); stats.bytes = stats.cpuBytes = 0; }
Stats GetStats() noexcept { return stats; }
void SetEnabledForTests(bool value) noexcept { enabled = value; Clear(); }
void SetOutlineEnabledForTests(bool value) noexcept { outlineEnabled = value; Clear(); }

bool Reuse(MMDApp* app, unsigned char* model, float distance, OutlineUpdate updateOutline) noexcept {
    if (!enabled) return false;
    try {
        Plan plan;
        if (!Describe(app, model, distance, plan)) { Forget(model); return false; }
        auto& entry = entries[model];
        if (entry.key != plan.key) {
            DropCpuBody(entry);
            entry.wantsCpuBody = false;
            entry.key = std::move(plan.key); entry.valid = false; entry.stable = 0;
        } else if (entry.valid && Same(entry, plan)) {
            if (entry.outlineKey == plan.outlineKey) { ++stats.hits; return true; }
            if (outlineEnabled && updateOutline && !entry.cpuBody.empty() &&
                updateOutline(app, model, distance, entry.cpuBody.data(), entry.cpuBody.size())) {
                entry.outlineKey = std::move(plan.outlineKey);
                ++stats.outlineHits; return true;
            }
            entry.wantsCpuBody = true;
        }
        entry.valid = false;
        // Foreign geometry and the comparison control retain the full-input gate.
        if ((!plan.hostOwned || !outlineEnabled) && entry.outlineKey != plan.outlineKey) {
            entry.stable = 0; entry.outlineKey = std::move(plan.outlineKey);
        }
        if (entry.stable < 2) ++entry.stable;
        ++stats.misses;
    } catch (...) { Forget(model); }
    return false;
}

void RememberCpuBody(unsigned char* model, const void* vertices, std::size_t count, std::size_t stride) noexcept {
    if (!enabled || !outlineEnabled || !vertices || stride < 6 * sizeof(float) ||
        !loadedModels.count(model) || count > perModelLimit / sizeof(OutlineVertex)) return;
    const auto found = entries.find(model);
    if (found == entries.end() || found->second.stable < 2 || !found->second.wantsCpuBody) return;
    auto& entry = found->second;
    const auto bytes = count * sizeof(OutlineVertex);
    if (bytes > totalLimit - stats.bytes - stats.cpuBytes) return;
    try {
        std::vector<OutlineVertex> body(count);
        const auto* source = static_cast<const unsigned char*>(vertices);
        const auto& state = *mdl::Mdl(model);
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(&body[i], source + i * stride, 6 * sizeof(float));
            if (state.physicsMode == 2) {
                const auto& vertex = state.pmxVertices[i];
                if (vertex.materialIndex & 0x80000000u) return;
                body[i].edgeScale = vertex.edgeScale;
                body[i].material = vertex.materialIndex | (vertex.hasPmxEdgeData ? 0u : 0x80000000u);
            } else {
                body[i].edgeScale = 1;
                body[i].material = state.rawVertices[i].edgeDisabled ? 0x80000000u : 0u;
            }
        }
        DropCpuBody(entry);
        entry.cpuBody = std::move(body); stats.cpuBytes += bytes;
    } catch (...) { DropCpuBody(entry); }
}

void Remember(MMDApp* app, unsigned char* model, float distance) noexcept {
    if (!enabled) return;
    const auto found = entries.find(model);
    if (found == entries.end() || found->second.stable < 2) return;
    // Two matching poses are required before copying large input tables. A
    // moving camera/character pays the small-key check, not a snapshot copy.
    try {
        Plan plan;
        if (!Describe(app, model, distance, plan)) { Forget(model); return; }
        auto& entry = found->second;
        if (entry.key != plan.key || plan.bytes > totalLimit - stats.cpuBytes - (stats.bytes - entry.bytes)) return;
        // Keep accounting correct if allocating a range throws.
        stats.bytes -= entry.bytes; entry.bytes = 0;
        entry.ranges.resize(plan.ranges.size());
        for (std::size_t i = 0; i < plan.ranges.size(); ++i) {
            auto& saved = entry.ranges[i]; const auto& source = plan.ranges[i];
            saved.address = source.address;
            if (saved.bytes.size() != source.size)
                saved.bytes = std::vector<unsigned char>(source.size);
            std::memcpy(saved.bytes.data(), source.address, source.size);
        }
        entry.bytes = plan.bytes; stats.bytes += entry.bytes;
        entry.outlineKey = std::move(plan.outlineKey);
        entry.valid = true; ++stats.stores;
    } catch (...) { Forget(model); }
}
}
