#pragma once
#include "common.hpp"
#include <mms/scene.h>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

namespace mikudancestudio::plugins {
struct IdLess { bool operator()(MmsId a, MmsId b) const { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); } };
inline bool Same(MmsId a, MmsId b) { return a.lo == b.lo && a.hi == b.hi; }
inline bool Empty(MmsId a) { return !a.lo && !a.hi; }
inline bool Same(MmsObjectRef a, MmsObjectRef b) { return Same(a.id, b.id) && a.generation == b.generation; }
MmsTime Normalize(MmsTime);
// Built on the UI thread, frozen before publication, owned through leases.
// No native pointers or callbacks occur in the public records.
struct SceneData {
    struct Channels { MmsObjectV1 object{}; std::map<uint32_t, MmsSpan> values; };
    uint32_t stage = 0;
    MmsId layer{};
    MmsTime time{0,1};
    uint64_t revision = 1, structure = 1, pose = 1, supported = 0;
    std::vector<Channels> objects;
    std::deque<std::string> strings;
    std::vector<std::shared_ptr<void>> arrays;
    MmsString Keep(std::string value) { strings.push_back(std::move(value)); return Text(strings.back()); }
    template<class T> MmsSpan Keep(std::vector<T> value) {
        auto array = std::make_shared<std::vector<T>>(std::move(value));
        MmsSpan span{array->data(), array->size(), sizeof(T), 1}; arrays.push_back(array); return span;
    }
};
class SceneStore;
struct SceneClient { SceneStore* store = nullptr; std::atomic<bool> stopping{false}; };
class SceneStore {
    struct Lease { SceneClient* owner; std::shared_ptr<const SceneData> data; uint64_t channels; std::vector<MmsObjectV1> objects; };
    DWORD thread_ = GetCurrentThreadId();
    std::mutex mutex_;
    std::map<MmsHandle, std::shared_ptr<Lease>> leases_;
    MmsHandle next_ = 1;
    bool acquiring_ = false;
    std::shared_ptr<Lease> Find(SceneClient&, MmsHandle);
    MmsResult Acquire(SceneClient&, const MmsSceneQueryV1*, MmsSnapshotV1*);
public:
    using Capture = std::function<MmsResult(const MmsSceneQueryV1&, std::shared_ptr<const SceneData>&)>;
    explicit SceneStore(Capture capture, std::function<bool(MmsId)> known = {}) : capture_(std::move(capture)), known_(std::move(known)) {}
    MmsSceneApiV1 Api(SceneClient&);
    bool HasLeases(SceneClient&);
private:
    Capture capture_;
    std::function<bool(MmsId)> known_;
};
}
