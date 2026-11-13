#include "scene_identity.hpp"
#include <bcrypt.h>
#include <limits>
namespace mikumikustudio::plugins {
#if !MMD_ORIGINAL_PROFILE
MmsResult SceneIdentity::MeasureGetPreparation(const void* token,std::uint64_t& bytes)const noexcept{
    if(!token)return MMS_E_ARGUMENT;
    if(const auto found=entries_.find(token);found!=entries_.end()){
        if(!found->second.present)return MMS_E_STALE;
        bytes=0;return MMS_OK;
    }
    if(!nextGeneration_)return MMS_E_NOT_READY;
    if(entries_.size()==entries_.max_size()||known_.size()==known_.max_size())return MMS_E_MEMORY;
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L && !_HAS_CXX23
    using EntryNode=std::_Tree_node<decltype(entries_)::value_type,void*>;
    using KnownNode=std::_Tree_node<decltype(known_)::value_type,void*>;
    using Elements=decltype(Entry::elements);
    using ElementNode=std::_Tree_node<Elements::value_type,void*>;
    std::uint64_t measured=0;
    const auto allocation=[&](std::uint64_t amount,std::size_t alignment)noexcept{
#if defined(_M_IX86) || defined(_M_X64)
        if(amount>=std::_Big_allocation_threshold
#ifdef __cpp_aligned_new
            &&alignment<=__STDCPP_DEFAULT_NEW_ALIGNMENT__
#endif
        ){
            if(amount>(std::numeric_limits<std::size_t>::max)()-std::_Non_user_size)return false;
            amount+=std::_Non_user_size;
        }
#endif
        (void)alignment;
        if(amount>(std::numeric_limits<std::size_t>::max)()||amount>=UINT64_MAX-measured)return false;
        measured+=amount;return true;
    };
    // Get inserts one known-ID node and one entry node. Its stack Entry first
    // constructs an empty elements map; moving Entry into the registry buys a
    // second empty sentinel before swapping maps. Both global registry heads
    // already exist. No Element value is inserted and no identity is created
    // by this query. Use the actual private container node types, not layouts.
    if(!allocation(sizeof(KnownNode),alignof(KnownNode))||
        !allocation(sizeof(EntryNode),alignof(EntryNode))||
        !allocation(sizeof(ElementNode),alignof(ElementNode))||
        !allocation(sizeof(ElementNode),alignof(ElementNode)))return MMS_E_MEMORY;
#if _ITERATOR_DEBUG_LEVEL != 0
    // Empty-map construction and move construction each buy one proxy.
    if(!allocation(sizeof(std::_Container_proxy),alignof(std::_Container_proxy))||
        !allocation(sizeof(std::_Container_proxy),alignof(std::_Container_proxy)))return MMS_E_MEMORY;
#endif
    bytes=measured;return MMS_OK;
#else
    (void)bytes;return MMS_E_NOT_READY;
#endif
}
#endif
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
    const auto id = entry.object.id;
    known_.insert(id);
    try { return entries_.emplace(token, std::move(entry)).first->second; }
    catch (...) { known_.erase(id); throw; }
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
    std::set<MmsId, IdLess> pending;
    // Retire positional IDs omitted after adaptation. Ordinary element tables
    // do not need a second persistent copy in the known-object registry.
    for (const auto& object : objects) {
        if (!known_.count(object.entry.object.id)) pending.insert(object.entry.object.id);
        if (!object.restoreElements)
            for (const auto& element : object.entry.elements)
                if (!known_.count(element.second)) pending.insert(element.second);
    }
    // Keep every historical ID without cloning the process-lifetime set on
    // each load. Both sets use the same always-equal standard allocator and
    // nonthrowing integer comparator. Preflight merge's size check too: all
    // validation and allocation must finish before publishing either table.
    if (pending.size() > known_.max_size() - known_.size())
        throw std::length_error("scene identity registry capacity");
    entries_.swap(replacement); known_.merge(pending); nextGeneration_ = generation;
}
}
