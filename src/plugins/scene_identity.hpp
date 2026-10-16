#pragma once
#include "scene_store.hpp"
#include <set>

namespace mikudancestudio::plugins {
class SceneIdentity {
public:
    struct Entry { MmsObjectRef object{}; std::map<uint64_t, MmsId> elements; };
    struct Restore { const void* token; Entry entry; };
    const Entry& Get(const void* token);
    MmsId Element(const void* token, uint32_t kind, uint32_t sourceIndex);
    bool Known(MmsId id) const { return known_.count(id) != 0; }
    void Forget(const void* token) { entries_.erase(token); }
    void Clear() { entries_.clear(); }
    // Validates and allocates the entire replacement before changing identity.
    // Persisted IDs survive reopening; runtime generations never do.
    void Replace(const std::vector<Restore>&);
private:
    std::map<const void*, Entry> entries_;
    std::set<MmsId, IdLess> known_;
    uint64_t nextGeneration_ = 1;
    MmsId NewId();
};
}
