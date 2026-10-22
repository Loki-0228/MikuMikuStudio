#include "scene_identity.hpp"
#include <bcrypt.h>
namespace mikumikustudio::plugins {
MmsId SceneIdentity::NewId() {
    MmsId id{};
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&id), sizeof(id), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("scene identity entropy unavailable");
    } while (Empty(id) || known_.count(id));
    return id;
}
const SceneIdentity::Entry& SceneIdentity::Get(const void* token) {
    if (!token) throw std::invalid_argument("null scene object");
    auto found = entries_.find(token);
    if (found != entries_.end()) {
        if (!found->second.present) throw std::invalid_argument("absent scene object");
        return found->second;
    }
    if (!nextGeneration_) throw std::overflow_error("scene generation exhausted");
    Entry entry; entry.object = {NewId(), nextGeneration_++};
    known_.insert(entry.object.id);
    return entries_.emplace(token, std::move(entry)).first->second;
}
const SceneIdentity::Entry* SceneIdentity::Retained(const void* token) const noexcept {
    const auto found = entries_.find(token);
    return found == entries_.end() ? nullptr : &found->second;
}
const SceneIdentity::Entry& SceneIdentity::CreateAbsent(const void* token) {
    if (!token || entries_.count(token)) throw std::invalid_argument("import identity already registered");
    if (!nextGeneration_) throw std::overflow_error("scene generation exhausted");
    Entry entry; entry.object = {NewId(), nextGeneration_++}; entry.present = entry.published = false;
    const auto id = entry.object.id;
    known_.insert(id);
    try { return entries_.emplace(token, std::move(entry)).first->second; }
    catch (...) { known_.erase(id); throw; }
}
SceneIdentity::Presence SceneIdentity::PreparePresence(const void* token, bool present) {
    const auto* entry = Retained(token);
    if (!entry || entry->present == present) throw std::invalid_argument("scene presence transition");
    if (!nextGeneration_) throw std::overflow_error("scene generation exhausted");
    Presence change; change.owner_ = this; change.token_ = token; change.before_ = entry->object;
    // Reserve a generation, not a live state change. Cancelled preparations
    // leave gaps; tickets are never reused. Independent objects can therefore
    // be prepared together and published in either order without collision.
    change.next_ = nextGeneration_++; change.beforePresent_ = entry->present; change.afterPresent_ = present;
    return change;
}
bool SceneIdentity::Presence::IsCurrent() const noexcept {
    if (!owner_) return false;
    const auto* entry = owner_->Retained(token_);
    return entry && Same(entry->object, before_) && entry->present == beforePresent_;
}
bool SceneIdentity::Presence::Publish() noexcept {
    if (!IsCurrent()) return false;
    auto& entry = owner_->entries_.find(token_)->second;
    entry.object.generation = next_;
    entry.present = afterPresent_; entry.published = entry.published || afterPresent_;
    owner_ = nullptr; return true;
}
MmsId SceneIdentity::Element(const void* token, uint32_t kind, uint32_t index) {
    Get(token); auto& values = entries_.at(token).elements;
    const uint64_t key = uint64_t(kind) << 32 | index;
    auto found = values.find(key); if (found != values.end()) return found->second;
    return values.emplace(key, NewId()).first->second;
}
void SceneIdentity::Replace(const std::vector<Restore>& objects) {
    std::map<const void*, Entry> replacement;
    std::set<MmsId, IdLess> ids;
    auto generation = nextGeneration_;
    for (const auto& object : objects) {
        if (!object.token || Empty(object.entry.object.id) || !ids.insert(object.entry.object.id).second || !generation)
            throw std::invalid_argument("invalid restored object identity");
        auto entry = object.entry; entry.object.generation = generation++; entry.present = entry.published = true;
        for (const auto& value : entry.elements)
            if (Empty(value.second) || !ids.insert(value.second).second) throw std::invalid_argument("duplicate element identity");
        if (!object.restoreElements) entry.elements.clear();
        if (!replacement.emplace(object.token, std::move(entry)).second) throw std::invalid_argument("duplicate restored object");
    }
    auto known = known_;
    // Retire positional IDs omitted after adaptation. Ordinary element tables
    // do not need a second persistent copy in the known-object registry.
    for (const auto& object : objects) {
        known.insert(object.entry.object.id);
        if (!object.restoreElements)
            for (const auto& element : object.entry.elements) known.insert(element.second);
    }
    entries_.swap(replacement); known_.swap(known); nextGeneration_ = generation;
}
}
