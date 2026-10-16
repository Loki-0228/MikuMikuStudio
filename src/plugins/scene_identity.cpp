#include "scene_identity.hpp"
#include <bcrypt.h>
namespace mikudancestudio::plugins {
MmsId SceneIdentity::NewId() {
    MmsId id{};
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&id), sizeof(id), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("scene identity entropy unavailable");
    } while (Empty(id));
    return id;
}
const SceneIdentity::Entry& SceneIdentity::Get(const void* token) {
    if (!token) throw std::invalid_argument("null scene object");
    auto found = entries_.find(token); if (found != entries_.end()) return found->second;
    if (!nextGeneration_) throw std::overflow_error("scene generation exhausted");
    Entry entry; entry.object = {NewId(), nextGeneration_++};
    known_.insert(entry.object.id);
    return entries_.emplace(token, std::move(entry)).first->second;
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
        auto entry = object.entry; entry.object.generation = generation++;
        for (const auto& value : entry.elements)
            if (Empty(value.second) || !ids.insert(value.second).second) throw std::invalid_argument("duplicate element identity");
        if (!replacement.emplace(object.token, std::move(entry)).second) throw std::invalid_argument("duplicate restored object");
    }
    auto known = known_;
    for (const auto& entry : replacement) known.insert(entry.second.object.id);
    entries_.swap(replacement); known_.swap(known); nextGeneration_ = generation;
}
}
