#pragma once
#include "scene_store.hpp"
#include <set>

namespace mikudancestudio::plugins {
class SceneIdentity {
public:
    struct Entry { MmsObjectRef object{}; std::map<uint64_t, MmsId> elements; bool present = true; };
    struct Restore { const void* token; Entry entry; };
    class Presence {
        friend class SceneIdentity;
        SceneIdentity* owner_ = nullptr;
        const void* token_ = nullptr;
        MmsObjectRef before_{};
        uint64_t next_ = 0;
        bool beforePresent_ = false, afterPresent_ = false;
    public:
        bool IsCurrent() const noexcept;
        bool Publish() noexcept;
    };
    const Entry& Get(const void* token);
    // Retained identity for an undoable absent object. Get/Element refuse it;
    // history can inspect it without making the object present again.
    const Entry* Retained(const void* token) const noexcept;
    Presence PreparePresence(const void* token, bool present);
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
