#pragma once
#include "scene_store.hpp"
#include <mms/evaluation.h>

namespace mikumikustudio::plugins {
// Built only by a trusted UI-thread host. The callback owns immutable sampling
// data; it must never capture a live model, plugin code or caller-owned memory.
struct EvaluationData {
    MmsObjectRef object{};
    uint64_t hostRevision = 0, retainedBytes = 0;
    std::vector<MmsEvaluationBoneV1> bones;
    std::function<MmsResult(uint32_t index, double frame, MmsBoneChannelsV1&)> sample;
};
class EvaluationStore {
public:
    struct Client {
        EvaluationStore* owner = nullptr; // Set once before publication.
        bool retired = false;
        uint32_t handles = 0, active = 0;
        uint64_t bytes = 0;
        Client() = default;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
    };
    using Capture = std::function<MmsResult(const MmsEvaluationSourceQueryV1&,
        uint64_t maxBytes, std::shared_ptr<const EvaluationData>&)>;
    explicit EvaluationStore(Capture);
    ~EvaluationStore();
    EvaluationStore(const EvaluationStore&) = delete;
    EvaluationStore& operator=(const EvaluationStore&) = delete;
    MmsEvaluationApiV1 Api(Client&);
    // Retire rejects new operations except release. Existing source handles
    // and admitted calls pin the client until plugins drain them and join work.
    void Retire(Client&) noexcept;
    bool HasLeases(Client&) const;
private:
    struct Payload;
    struct Entry { Client* owner; std::shared_ptr<const Payload> data; };
    Capture capture_;
    DWORD thread_ = GetCurrentThreadId();
    mutable std::mutex mutex_;
    std::map<MmsHandle, Entry> sources_;
    uint64_t next_ = (uint64_t(1) << 62) | 1u, captureId_ = 1, totalBytes_ = 0;
    bool acquiring_ = false;
    MmsResult Capabilities(Client&, MmsEvaluationCapsV1*);
    MmsResult Create(Client&, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult Describe(Client&, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult BoneAt(Client&, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult Sample(Client&, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult Retain(Client&, MmsHandle, MmsHandle*);
    MmsResult Release(Client&, MmsHandle);
    void Settle(const std::shared_ptr<const Payload>&, Client&) noexcept;
};
}
