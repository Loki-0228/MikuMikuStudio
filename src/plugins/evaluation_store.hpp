#pragma once
#include "scene_store.hpp"
#include <mms/evaluation.h>
#include "mikumikustudio/evaluation_time.hpp"

namespace mikumikustudio::plugins {
// Built only by a trusted UI-thread host. The callback owns immutable sampling
// data; it must never capture a live model, plugin code or caller-owned memory.
struct EvaluationData {
    MmsObjectRef object{};
    uint64_t hostRevision = 0, retainedBytes = 0;
    uint32_t channels = MMS_EVALUATION_CAP_BONE_CHANNELS;
    std::vector<MmsEvaluationBoneV1> bones;
    std::function<MmsResult(uint32_t index, double frame, MmsBoneChannelsV1&)> sample;
    std::vector<MmsEvaluationMorphV1> morphs;
    std::function<MmsResult(uint32_t index, double frame, MmsMorphWeightV1&)> sampleMorph;
    std::vector<MmsEvaluationIkV1> ik;
    std::vector<MmsEvaluationSelectorV1> selectors;
    std::function<MmsResult(double frame, MmsDisplayStateV1&)> sampleVisibility;
    std::function<MmsResult(uint32_t index, double frame, MmsIkStateV1&)> sampleIk;
    std::function<MmsResult(uint32_t index, double frame, MmsParentSelectionV1&)> sampleParent;
    // Canonical parent state when supplied; otherwise adapt the legacy host
    // callback. Both public sampling versions use one validation/lease path.
    std::function<MmsResult(uint32_t index, double frame, MmsParentStateV1&)> sampleParentState;
};
// Trusted host data. Both scene payloads hold the same immutable identity map;
// each payload's retainedBytes accounts that map independently.
struct EvaluationSceneModelData {
    MmsObjectRef object{};
    std::vector<MmsId> bones;
};
using EvaluationSceneModels = std::vector<EvaluationSceneModelData>;
using EvaluationMaterialIds = std::vector<std::vector<MmsId>>;
struct EvaluationSceneFrameData {
    std::shared_ptr<const EvaluationSceneModels> models;
    std::vector<uint8_t> visibility;
    uint64_t retainedBytes = 0;
    std::function<MmsResult(uint32_t model, uint32_t bone, MmsEvaluationScenePoseV1&)> sample;
    std::vector<uint32_t> meshVertexCounts;
    std::vector<uint32_t> meshAdditionalUvCounts;
    std::function<MmsResult(uint32_t model, uint32_t vertex, MmsEvaluationMeshVertexV1&)> meshVertex;
    std::shared_ptr<const EvaluationMaterialIds> materialIds;
    std::function<MmsResult(uint32_t model, uint32_t material, MmsEvaluationMaterialV1&)> material;
};
struct EvaluationSceneSourceData {
    std::shared_ptr<const EvaluationSceneModels> models;
    uint64_t hostRevision = 0, retainedBytes = 0;
    std::function<MmsResult(uint32_t model, MmsEvaluationMeshInfoV1&)> meshInfo;
    std::function<MmsResult(uint32_t model, uint32_t index, uint32_t&)> meshIndex;
    std::function<MmsResult(uint32_t model, uint32_t material, MmsEvaluationMeshMaterialV1&)> meshMaterial;
    std::function<MmsResult(uint32_t model, uint32_t vertex, MmsEvaluationMeshVertexV1&)> meshBindVertex;
    std::shared_ptr<const EvaluationMaterialIds> materialIds;
    std::function<MmsResult(uint32_t,uint32_t,MmsEvaluationMaterialReferencesV1&)> references;
    std::function<MmsResult(uint32_t,MmsEvaluationToonTableV1&)> toonTable;
    std::function<MmsResult(const EvaluationTime&, uint64_t maxFrameBytes,
        uint64_t maxWorkspaceBytes, std::shared_ptr<const EvaluationSceneFrameData>&)> evaluate;
};
class EvaluationStore {
public:
    struct Client {
        EvaluationStore* owner = nullptr; // Set once before publication.
        bool retired = false;
        uint32_t handles = 0, active = 0;
        uint64_t bytes = 0;
        uint64_t workspaceBytes = 0;
        Client() = default;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
    };
    using Capture = std::function<MmsResult(const MmsEvaluationSourceQueryV1&,
        uint64_t maxBytes, std::shared_ptr<const EvaluationData>&)>;
    explicit EvaluationStore(Capture);
    using CaptureChannels = std::function<MmsResult(const MmsEvaluationSourceQueryV1&,
        uint32_t channels, uint64_t maxBytes, std::shared_ptr<const EvaluationData>&)>;
    using CaptureScene = std::function<MmsResult(const MmsEvaluationSceneQueryV1&,
        uint64_t maxBytes, std::shared_ptr<const EvaluationSceneSourceData>&)>;
    explicit EvaluationStore(CaptureChannels, uint32_t supportedChannels = 3, CaptureScene scene = {}, CaptureScene meshScene = {}, CaptureScene materialScene = {}, CaptureScene referenceScene = {});
    ~EvaluationStore();
    EvaluationStore(const EvaluationStore&) = delete;
    EvaluationStore& operator=(const EvaluationStore&) = delete;
    MmsEvaluationApiV1 Api(Client&);
    MmsEvaluationApiV1_1 Api11(Client&);
    MmsEvaluationApiV1_2 Api12(Client&);
    MmsEvaluationApiV1_3 Api13(Client&);
    MmsEvaluationApiV1_4 Api14(Client&);
    MmsEvaluationApiV1_5 Api15(Client&);
    MmsEvaluationApiV1_6 Api16(Client&);
    MmsEvaluationApiV1_7 Api17(Client&);
    // Retire rejects new operations except release. Existing source handles
    // and admitted calls pin the client until plugins drain them and join work.
    void Retire(Client&) noexcept;
    bool HasLeases(Client&) const;
private:
    struct Payload;
    struct Entry { Client* owner; std::shared_ptr<const Payload> data; };
    Capture capture_;
    CaptureChannels captureChannels_;
    CaptureScene captureScene_;
    CaptureScene captureMeshScene_;
    CaptureScene captureMaterialScene_;
    CaptureScene captureReferenceScene_;
    uint32_t supportedChannels_ = MMS_EVALUATION_CAP_BONE_CHANNELS;
    DWORD thread_ = GetCurrentThreadId();
    mutable std::mutex mutex_;
    std::map<MmsHandle, Entry> sources_;
    uint64_t next_ = (uint64_t(1) << 62) | 1u, captureId_ = 1, totalBytes_ = 0;
    uint64_t totalWorkspaceBytes_ = 0;
    bool acquiring_ = false;
    MmsResult Capabilities(Client&, MmsEvaluationCapsV1*);
    MmsResult Create(Client&, const MmsEvaluationSourceQueryV1*, MmsHandle*);
    MmsResult CreateChannels(Client&, const MmsEvaluationSourceQueryV1*, uint32_t, MmsHandle*);
    MmsResult Describe(Client&, MmsHandle, MmsEvaluationSourceInfoV1*);
    MmsResult DescribeChannels(Client&, MmsHandle, MmsEvaluationSourceInfoV1_1*);
    MmsResult BoneAt(Client&, MmsHandle, uint32_t, MmsEvaluationBoneV1*);
    MmsResult MorphAt(Client&, MmsHandle, uint32_t, MmsEvaluationMorphV1*);
    MmsResult Sample(Client&, MmsHandle, MmsId, MmsTime, MmsBoneChannelsV1*);
    MmsResult SampleMorph(Client&, MmsHandle, MmsId, MmsTime, MmsMorphWeightV1*);
    MmsResult DescribeDisplay(Client&, MmsHandle, MmsEvaluationSourceInfoV1_2*);
    MmsResult IkAt(Client&, MmsHandle, uint32_t, MmsEvaluationIkV1*);
    MmsResult SelectorAt(Client&, MmsHandle, uint32_t, MmsEvaluationSelectorV1*);
    MmsResult SampleVisibility(Client&, MmsHandle, MmsTime, MmsDisplayStateV1*);
    MmsResult SampleIk(Client&, MmsHandle, uint32_t, MmsTime, MmsIkStateV1*);
    MmsResult SampleParent(Client&, MmsHandle, uint32_t, MmsTime, MmsParentSelectionV1*);
    MmsResult SampleParentState(Client&, MmsHandle, uint32_t, MmsTime, MmsParentStateV1*);
    MmsResult ReadDisplay(Client&, MmsHandle, uint32_t kind, uint32_t index, MmsTime, void*);
    MmsResult SceneCapabilities(Client&, MmsEvaluationSceneCapsV1*);
    MmsResult CreateScene(Client&, const MmsEvaluationSceneQueryV1*, MmsHandle*, uint32_t kind = 0);
    MmsResult MeshCapabilities(Client&, MmsEvaluationMeshCapsV1*);
    MmsResult DescribeMesh(Client&, MmsHandle, uint32_t, MmsEvaluationMeshInfoV1*);
    MmsResult ReadMesh(Client&, MmsHandle, uint32_t, uint32_t, uint32_t, void*, uint32_t);
    MmsResult MaterialCapabilities(Client&, MmsEvaluationMaterialCapsV1*);
    MmsResult MaterialAt(Client&, MmsHandle, uint32_t, uint32_t, MmsEvaluationMaterialIdV1*);
    MmsResult ReferenceCapabilities(Client&, MmsEvaluationReferenceCapsV1*);
    MmsResult ReadReference(Client&, MmsHandle, uint32_t, uint32_t, void*, bool);
    void FinishMeshRead(std::shared_ptr<const Payload>&, Client&, uint64_t workspace) noexcept;
    MmsResult DescribeScene(Client&, MmsHandle, MmsEvaluationSceneInfoV1*);
    MmsResult SceneModelAt(Client&, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult SceneBoneAt(Client&, MmsHandle, uint32_t, uint32_t, MmsEvaluationSceneBoneV1*);
    MmsResult EvaluateFrame(Client&, const MmsEvaluationFrameQueryV1*, MmsHandle*);
    MmsResult DescribeFrame(Client&, MmsHandle, MmsEvaluationFrameInfoV1*);
    MmsResult FrameModelAt(Client&, MmsHandle, uint32_t, MmsEvaluationSceneModelV1*);
    MmsResult ReadBonePose(Client&, MmsHandle, uint32_t, uint32_t, MmsEvaluationScenePoseV1*);
    MmsResult Retain(Client&, MmsHandle, MmsHandle*);
    MmsResult Release(Client&, MmsHandle);
};
}
