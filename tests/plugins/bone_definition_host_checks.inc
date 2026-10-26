#include "mikumikustudio/bone_definition_snapshot.hpp"
#include <tuple>

#if !MMD_ORIGINAL_PROFILE
namespace bone_definition_host_checks {
namespace {
struct Expected {
    std::vector<BoneDefinition> bones;
    std::vector<std::vector<BoneDefinitionIkLink>> pmxLinks;
    std::vector<BoneDefinitionIk> chains;
    std::vector<std::vector<uint32_t>> links;
    std::vector<int32_t> selectors;
    uint8_t physicsMode = 0;
    int32_t maxBoneLayer = 0;
};
template<class T, size_t N>
std::array<T, N> CopyArray(const T (&source)[N]) {
    std::array<T, N> result{};
    std::copy(source, source + N, result.begin());
    return result;
}
Expected ReadExpected(const mdl::ModelRecord& model) {
    Expected result;
    result.physicsMode = model.physicsMode; result.maxBoneLayer = model.maxBoneLayer;
    for (uint32_t i = 0; i < model.boneCount; ++i) {
        const auto& source = model.boneTable[i];
        BoneDefinition bone{};
        bone.name = CopyArray(source.name); bone.nameEn = CopyArray(source.nameEn);
        bone.parent = source.parent; bone.position = CopyArray(source.position);
        bone.type = source.type; bone.flags = source.flags; bone.layer = source.layer;
        bone.hasFlag = source.hasFlag; bone.hasRigidBody = source.hasRigidBody;
        bone.tailBone = source.tailBone; bone.tailIdx = source.tailIdx; bone.tailOffset = CopyArray(source.tailOffset);
        bone.inheritRatio = source.inheritRatio; bone.axis = CopyArray(source.axis); bone.localAxes = CopyArray(source.localAxes);
        bone.extParent = source.extParent; bone.slotIndex = source.slotIndex; bone.twistEnable = source.twistEnable;
        bone.ikLimitMin = CopyArray(source.ikLimitMin); bone.ikLimitMax = CopyArray(source.ikLimitMax);
        bone.ikTarget = source.ikTarget; bone.ikLoop = source.ikLoop; bone.ikAngle = source.ikAngle;
        std::vector<BoneDefinitionIkLink> pmx;
        if (model.physicsMode == 2 && (source.flags & mdl::kBoneFlagIk)) {
            Check(source.ikLinkCount > 0 && source.ikLinks, "loaded PMX IK source retains its links");
            bone.pmxLinkCount = uint32_t(source.ikLinkCount);
            for (uint32_t j = 0; j < bone.pmxLinkCount; ++j) {
                const auto& raw = source.ikLinks[j];
                BoneDefinitionIkLink link{};
                link.bone = raw.boneIndex; link.hasLimits = raw.hasLimits;
                link.minimum = CopyArray(raw.minimum); link.maximum = CopyArray(raw.maximum);
                pmx.push_back(link);
            }
        }
        result.bones.push_back(bone); result.pmxLinks.push_back(std::move(pmx));
    }
    for (uint32_t i = 0; i < model.ikChainCount; ++i) {
        const auto& raw = model.ikChains[i];
        Check(raw.linkCount && raw.links, "loaded runtime IK chain has real links");
        BoneDefinitionIk chain{};
        chain.bone = raw.boneIndex; chain.target = raw.targetBone; chain.linkCount = raw.linkCount;
        chain.iterations = raw.iterations; chain.maxAngle = raw.maxAngle;
        result.chains.push_back(chain);
        result.links.emplace_back(raw.links, raw.links + raw.linkCount);
    }
    const auto* order = static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
    for (uint32_t i = 0; i < model.boneOrderCount; ++i) result.selectors.push_back(order[i].boneIndex);
    return result;
}
bool SameDefinition(const BoneDefinition& a, const BoneDefinition& b) {
    return std::tie(a.name, a.nameEn, a.parent, a.position, a.type, a.flags, a.layer, a.hasFlag, a.hasRigidBody,
        a.tailBone, a.tailIdx, a.tailOffset, a.inheritRatio, a.axis, a.localAxes, a.extParent, a.slotIndex,
        a.twistEnable, a.ikLimitMin, a.ikLimitMax, a.ikTarget, a.ikLoop, a.ikAngle, a.pmxLinkCount) ==
        std::tie(b.name, b.nameEn, b.parent, b.position, b.type, b.flags, b.layer, b.hasFlag, b.hasRigidBody,
        b.tailBone, b.tailIdx, b.tailOffset, b.inheritRatio, b.axis, b.localAxes, b.extParent, b.slotIndex,
        b.twistEnable, b.ikLimitMin, b.ikLimitMax, b.ikTarget, b.ikLoop, b.ikAngle, b.pmxLinkCount);
}
bool Matches(const BoneDefinitionSnapshot& snapshot, const Expected& expected) {
    if (snapshot.BoneCount() != expected.bones.size() || snapshot.IkCount() != expected.chains.size() ||
        snapshot.SelectorCount() != expected.selectors.size() || snapshot.PhysicsMode() != expected.physicsMode ||
        snapshot.MaxBoneLayer() != expected.maxBoneLayer || !snapshot.RetainedBytes()) return false;
    for (uint32_t i = 0; i < expected.bones.size(); ++i) {
        BoneDefinition bone{};
        if (!snapshot.BoneAt(i, bone) || !SameDefinition(bone, expected.bones[i])) return false;
        for (uint32_t j = 0; j < expected.pmxLinks[i].size(); ++j) {
            BoneDefinitionIkLink link{};
            const auto& wanted = expected.pmxLinks[i][j];
            if (!snapshot.PmxLinkAt(i, j, link) || link.bone != wanted.bone || link.hasLimits != wanted.hasLimits ||
                link.minimum != wanted.minimum || link.maximum != wanted.maximum) return false;
        }
        BoneDefinitionIkLink absent{}; absent.bone = 123456;
        if (snapshot.PmxLinkAt(i, uint32_t(expected.pmxLinks[i].size()), absent) || absent.bone != 123456) return false;
    }
    for (uint32_t i = 0; i < expected.chains.size(); ++i) {
        BoneDefinitionIk chain{}; const auto& wanted = expected.chains[i];
        if (!snapshot.IkAt(i, chain) || chain.bone != wanted.bone || chain.target != wanted.target ||
            chain.linkCount != wanted.linkCount || chain.iterations != wanted.iterations || chain.maxAngle != wanted.maxAngle) return false;
        for (uint32_t j = 0; j < expected.links[i].size(); ++j) {
            uint32_t link = 123456;
            if (!snapshot.IkLinkAt(i, j, link) || link != expected.links[i][j]) return false;
        }
    }
    for (uint32_t i = 0; i < expected.selectors.size(); ++i) {
        int32_t bone = 123456;
        if (!snapshot.SelectorBone(i, bone) || bone != expected.selectors[i]) return false;
    }
    return true;
}
std::vector<unsigned char> SourceBytes(const mdl::ModelRecord& model) {
    std::vector<unsigned char> bytes;
    const auto append = [&](const void* source, size_t size) {
        if (!size) return;
        const auto* begin = static_cast<const unsigned char*>(source);
        bytes.insert(bytes.end(), begin, begin + size);
    };
    append(&model, sizeof(model));
    append(model.boneTable, model.boneCount * sizeof(mdl::BoneRecord));
    append(model.ikChains, model.ikChainCount * sizeof(mdl::IkChain));
    append(model.boneOrderTable, model.boneOrderCount * sizeof(mdl::BoneOrderEntry));
    for (uint32_t i = 0; i < model.ikChainCount; ++i)
        append(model.ikChains[i].links, model.ikChains[i].linkCount * sizeof(uint16_t));
    if (model.physicsMode == 2) for (uint32_t i = 0; i < model.boneCount; ++i) {
        const auto& bone = model.boneTable[i];
        if (bone.flags & mdl::kBoneFlagIk) append(bone.ikLinks, size_t(bone.ikLinkCount) * sizeof(mdl::PmxIkLinkRecord));
    }
    return bytes;
}
void Workers(const BoneDefinitionSnapshot& old, const Expected& original,
    const BoneDefinitionSnapshot& newer, const Expected& edited) {
    bool firstOk = true, secondOk = true;
    const auto read = [&](bool& ok) {
        for (unsigned i = 0; i < 128; ++i) ok = Matches(old, original) && Matches(newer, edited) && ok;
    };
    std::thread first([&] { read(firstOk); });
    std::thread second;
    try { second = std::thread([&] { read(secondOk); }); }
    catch (...) { first.join(); throw; }
    first.join(); second.join();
    Check(firstOk && secondOk, "concurrent definition reads retain both independent revisions");
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "real PMX or PMD bone definition fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load bone definition fixture");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount && model.boneTable && model.ikChainCount && model.ikChains &&
        model.boneOrderCount > 1 && model.boneOrderTable, "real loader produced bones IK and local selectors");
    const auto original = ReadExpected(model);
    Check(original.selectors[0] == -1, "native global selector is preserved as minus one");
    size_t pmxLinkCount = 0;
    for (const auto& links : original.pmxLinks) pmxLinkCount += links.size();
    Check(model.physicsMode == 2 ? pmxLinkCount > 0 : pmxLinkCount == 0,
        "PMX exposes original IK links while PMD uses runtime chains only");
    app->CurrentFrame() = 3; app->SceneModified() = 0;
    const auto before = SourceBytes(model);
    auto old = BoneDefinitionSnapshot::Capture(raw);
    Check(old && Matches(*old, original), "frozen definitions match every loaded static field and nested link");
    Check(before == SourceBytes(model) && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "capture and reads preserve full model bone chain selector and editing state");

    const char savedName = model.boneTable[0].name[0];
    model.boneTable[0].name[0] = savedName == 'X' ? 'Y' : 'X';
    const auto edited = ReadExpected(model);
    const auto pending = SourceBytes(model);
    std::unique_ptr<BoneDefinitionSnapshot> newer;
    try { newer = BoneDefinitionSnapshot::Capture(raw); }
    catch (...) { model.boneTable[0].name[0] = savedName; throw; }
    const bool unchanged = pending == SourceBytes(model);
    model.boneTable[0].name[0] = savedName;
    Check(newer && unchanged && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "second capture reads pending static edit without modifying the source");
    Check(!Matches(*old, edited) && !Matches(*newer, original), "two captures retain distinct static name revisions");
    Workers(*old, original, *newer, edited);
    Check(before == SourceBytes(model), "temporary static edit restored before saving the project");

    const auto project = std::filesystem::absolute(L"bone-definition.pmm");
    app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save real fixture in PMM");
    ResetAppState(app); Check(!app->ModelSlot(0), "destroy original definition source");
    Workers(*old, original, *newer, edited);
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0), "reload definition fixture from saved PMM");
    Workers(*old, original, *newer, edited);
    auto& reloaded = *mdl::Mdl(app->ModelSlot(0));
    const auto reloadExpected = ReadExpected(reloaded);
    const auto reloadBefore = SourceBytes(reloaded);
    auto reloadSnapshot = BoneDefinitionSnapshot::Capture(app->ModelSlot(0));
    Check(reloadSnapshot && Matches(*reloadSnapshot, reloadExpected) && reloadBefore == SourceBytes(reloaded),
        "reloaded model also captures real static definitions without writes");
    ResetAppState(app);
    Check(Matches(*reloadSnapshot, reloadExpected), "reloaded snapshot also survives model destruction");
    std::puts("Frozen bone definitions: real loader fields, nested links, read-only capture and independent lifetime passed");
}
}
#endif
