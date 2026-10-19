#include "scene_store.hpp"
#include <numeric>
#include <set>

namespace mikumikustudio::plugins {
MmsTime Normalize(MmsTime t) {
    if (!t.denominator) throw std::invalid_argument("zero time denominator");
    const uint64_t magnitude = t.numerator < 0 ? uint64_t(-(t.numerator + 1)) + 1 : uint64_t(t.numerator);
    const auto divisor = std::gcd(magnitude, t.denominator);
    const auto reduced = magnitude / divisor;
    if (t.numerator < 0) t.numerator = reduced == (uint64_t(1) << 63) ? INT64_MIN : -int64_t(reduced);
    else t.numerator = int64_t(reduced);
    t.denominator /= divisor; return t;
}
std::shared_ptr<SceneStore::Lease> SceneStore::Find(SceneClient& client, MmsHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = leases_.find(handle);
    return it != leases_.end() && it->second->owner == &client ? it->second : nullptr;
}
bool SceneStore::HasLeases(SceneClient& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : leases_) if (item.second->owner == &client) return true;
    return false;
}
MmsResult SceneStore::Acquire(SceneClient& client, const MmsSceneQueryV1* query, MmsSnapshotV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (client.stopping) return MMS_E_NOT_READY;
    if (acquiring_) return MMS_E_BUSY;
    if (!Valid(query) || !out || (query->flags & ~MMS_SCENE_CURRENT_TIME) || query->stage > MMS_STAGE_LAYER ||
        (query->channels & ~uint64_t(511)) || (query->stage == MMS_STAGE_LAYER ? Empty(query->layerId) : !Empty(query->layerId))) return MMS_E_ARGUMENT;
    const bool currentTime = (query->flags & MMS_SCENE_CURRENT_TIME) != 0;
    const auto time = currentTime ? MmsTime{} : Normalize(query->time);
    const auto& span = query->objects;
    if (span.count && (!span.data || span.count > 65536 || span.recordVersion != 1 ||
        span.stride < sizeof(MmsObjectRef) || span.stride > 65536 || span.count > SIZE_MAX / span.stride)) return MMS_E_ARGUMENT;
    acquiring_ = true;
    struct Guard { bool& value; ~Guard() { value = false; } } guard{acquiring_};
    std::shared_ptr<const SceneData> data;
    auto result = capture_(*query, data);
    if (result != MMS_OK) return result;
    if (!data || data->stage != query->stage || !Same(data->layer, query->layerId)) return MMS_E_FAILED;
    const auto publishedTime = Normalize(data->time);
    if (!currentTime && (time.numerator != publishedTime.numerator || time.denominator != publishedTime.denominator)) return MMS_E_NOT_READY;
    if ((query->channels & data->supported) != query->channels) return MMS_E_UNSUPPORTED;
    auto lease = std::make_shared<Lease>(); lease->owner = &client; lease->data = std::move(data); lease->channels = query->channels;
    std::set<MmsId, IdLess> requested;
    if (span.count) for (size_t n = 0; n < span.count; ++n) {
        MmsObjectRef object; std::memcpy(&object, static_cast<const unsigned char*>(span.data) + n * span.stride, sizeof(object));
        if (Empty(object.id) || !object.generation || !requested.insert(object.id).second) return MMS_E_ARGUMENT;
        auto it = std::find_if(lease->data->objects.begin(), lease->data->objects.end(), [&](const auto& record) { return Same(record.object.object.id, object.id); });
        if (it == lease->data->objects.end()) return known_ && known_(object.id) ? MMS_E_STALE : MMS_E_NOT_FOUND;
        if (!Same(it->object.object, object)) return MMS_E_STALE;
    }
    for (const auto& record : lease->data->objects)
        if (!span.count || requested.count(record.object.object.id)) lease->objects.push_back(record.object);
    std::lock_guard<std::mutex> lock(mutex_);
    if (leases_.size() >= 4096) return MMS_E_BUSY;
    const auto handle = next_++;
    leases_.emplace(handle, lease);
    *out = {Header<MmsSnapshotV1>(), handle, lease->data->revision, lease->data->structure, lease->data->pose};
    return MMS_OK;
}
MmsSceneApiV1 SceneStore::Api(SceneClient& client) {
    client.store = this;
    MmsSceneApiV1 api{}; api.h = Header<MmsSceneApiV1>(); api.context = &client;
    api.acquire = [](void* p, const MmsSceneQueryV1* q, MmsSnapshotV1* out) { return Boundary([&] { auto& c = *static_cast<SceneClient*>(p); return c.store->Acquire(c, q, out); }); };
    api.release = [](void* p, MmsHandle h) {
        auto& c = *static_cast<SceneClient*>(p); std::lock_guard<std::mutex> lock(c.store->mutex_);
        auto it = c.store->leases_.find(h); if (it != c.store->leases_.end() && it->second->owner == &c) c.store->leases_.erase(it);
    };
    api.objects = [](void* p, MmsHandle h, MmsSpan* out) { return Boundary([&]() -> MmsResult {
        auto& c = *static_cast<SceneClient*>(p); if (!out) return MMS_E_ARGUMENT;
        auto lease = c.store->Find(c, h); if (!lease) return MMS_E_STALE;
        *out = {lease->objects.data(), lease->objects.size(), sizeof(MmsObjectV1), 1}; return MMS_OK;
    }); };
    api.readChannel = [](void* p, MmsHandle h, MmsObjectRef object, uint32_t channel, MmsSpan* out) { return Boundary([&]() -> MmsResult {
        auto& c = *static_cast<SceneClient*>(p);
        if (!out || !channel || (channel & (channel - 1)) || (channel & ~511u)) return MMS_E_ARGUMENT;
        auto lease = c.store->Find(c, h); if (!lease) return MMS_E_STALE;
        if (!(lease->channels & channel)) return MMS_E_NOT_READY;
        auto selected = std::find_if(lease->objects.begin(), lease->objects.end(), [&](const auto& record) { return Same(record.object.id, object.id); });
        if (selected == lease->objects.end()) return MMS_E_NOT_FOUND;
        if (!Same(selected->object, object)) return MMS_E_STALE;
        auto record = std::find_if(lease->data->objects.begin(), lease->data->objects.end(), [&](const auto& r) { return Same(r.object.object, object); });
        auto value = record->values.find(channel);
        if (value == record->values.end()) return MMS_E_UNSUPPORTED;
        *out = value->second; return MMS_OK;
    }); };
    return api;
}
}
