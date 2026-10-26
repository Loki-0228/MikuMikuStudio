#include "mikumikustudio/morph_definition_snapshot.hpp"
#include <stdexcept>

#if !MMD_ORIGINAL_PROFILE
namespace morph_definition_probe {
namespace {
struct MorphExpected {
    MorphDefinition definition;
    std::vector<mdl::PmdVertexMorphEntry> vertices;
    std::vector<mdl::PmxBoneMorphEntry> bones;
    std::vector<mdl::PmxGroupMorphEntry> groups;
    std::array<std::vector<mdl::PmxUvMorphEntry>, 5> uv;
    std::vector<mdl::PmxMaterialMorphEntry> materials;
};
struct Expected {
    std::vector<MorphExpected> morphs;
    std::vector<mdl::PmdVertexMorphEntry> baseVertices;
    std::array<std::vector<mdl::PmxUvMorphEntry>, 5> baseUv;
    std::vector<mdl::MaterialMorphChannels> baseMaterials;
    uint32_t vertices = 0, bones = 0, materials = 0;
    uint8_t physicsMode = 0, additionalUv = 0;
};
template<class T> std::vector<T> CopyEntries(const T* data, size_t count) {
    if (!count) return {};
    Check(data != nullptr, "loaded morph definition table is present");
    return std::vector<T>(data, data + count);
}
Expected ReadExpected(const mdl::ModelRecord& model) {
    Expected result;
    result.vertices = model.vertexCount; result.bones = model.boneCount; result.materials = model.materialCount;
    result.physicsMode = model.physicsMode; result.additionalUv = model.pmxAdditionalUvCount;
    for (uint32_t i = 0; i < model.morphCount; ++i) {
        const auto& raw = model.morphs[i]; MorphExpected entry;
        auto& d = entry.definition;
        std::copy(raw.name, raw.name + 20, d.name.begin()); std::copy(raw.nameEn, raw.nameEn + 20, d.nameEn.begin());
        d.panel = uint8_t(raw.panel); d.type = raw.type;
        if (model.physicsMode != 2 || raw.type == 1) {
            d.vertexCount = uint32_t(raw.offsetCount); entry.vertices = CopyEntries(raw.vertexEntries, d.vertexCount);
        } else if (raw.type == 0) {
            d.groupCount = uint32_t(raw.groupCount); entry.groups = CopyEntries(raw.groupEntries, d.groupCount);
        } else if (raw.type == 2) {
            d.boneCount = uint32_t(raw.boneCount); entry.bones = CopyEntries(raw.boneEntries, d.boneCount);
        } else if (raw.type >= 3 && raw.type <= 7) {
            const auto family = raw.type - 3;
            d.uvCounts[family] = uint32_t(raw.uvCounts[family]);
            entry.uv[family] = CopyEntries(raw.uvEntries[family], d.uvCounts[family]);
        } else if (raw.type == 8) {
            d.materialCount = uint32_t(raw.materialCount); entry.materials = CopyEntries(raw.materialEntries, d.materialCount);
        } else Check(false, "fixture contains only supported PMX 2.0 definition types");
        result.morphs.push_back(std::move(entry));
    }
    result.baseVertices = CopyEntries(model.morph0Table, model.morph0Count);
    for (uint32_t family = 0; family < 5; ++family)
        result.baseUv[family] = CopyEntries(model.uvMorphTables.byFamily[family], size_t(model.uvMorphCounts.byFamily[family]));
    if (model.materialMorphPools.base) for (uint32_t i = 0; i < model.materialCount; ++i)
        result.baseMaterials.push_back(model.materialMorphPools.base[i].channels);
    return result;
}
template<class T> bool Equal(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool Equal(const mdl::PmxMaterialMorphEntry& a, const mdl::PmxMaterialMorphEntry& b) {
    return a.materialIndex == b.materialIndex && a.operation == b.operation && Equal(a.channels, b.channels);
}
bool Equal(const MorphDefinition& a, const MorphDefinition& b) {
    return a.name == b.name && a.nameEn == b.nameEn && a.panel == b.panel && a.type == b.type &&
        a.vertexCount == b.vertexCount && a.boneCount == b.boneCount && a.groupCount == b.groupCount &&
        a.materialCount == b.materialCount && a.uvCounts == b.uvCounts;
}
template<class T, class Read> bool EntriesMatch(const std::vector<T>& expected, Read read) {
    for (uint32_t i = 0; i < expected.size(); ++i) {
        T value{};
        if (!read(i, value) || !Equal(value, expected[i])) return false;
    }
    return true;
}
bool Matches(const MorphDefinitionSnapshot& snapshot, const Expected& expected) {
    if (snapshot.MorphCount() != expected.morphs.size() || snapshot.VertexCount() != expected.vertices ||
        snapshot.BoneCount() != expected.bones || snapshot.MaterialCount() != expected.materials ||
        snapshot.PhysicsMode() != expected.physicsMode || snapshot.AdditionalUvCount() != expected.additionalUv ||
        snapshot.BaseVertexCount() != expected.baseVertices.size() || snapshot.BaseMaterialCount() != expected.baseMaterials.size() ||
        !snapshot.RetainedBytes()) return false;
    for (uint32_t i = 0; i < expected.morphs.size(); ++i) {
        MorphDefinition definition{}; const auto& e = expected.morphs[i];
        if (!snapshot.MorphAt(i, definition) || !Equal(definition, e.definition) ||
            !EntriesMatch(e.vertices, [&](uint32_t j, auto& out) { return snapshot.VertexAt(i, j, out); }) ||
            !EntriesMatch(e.bones, [&](uint32_t j, auto& out) { return snapshot.BoneAt(i, j, out); }) ||
            !EntriesMatch(e.groups, [&](uint32_t j, auto& out) { return snapshot.GroupAt(i, j, out); }) ||
            !EntriesMatch(e.materials, [&](uint32_t j, auto& out) { return snapshot.MaterialAt(i, j, out); })) return false;
        for (uint32_t family = 0; family < 5; ++family)
            if (!EntriesMatch(e.uv[family], [&](uint32_t j, auto& out) { return snapshot.UvAt(i, family, j, out); })) return false;
    }
    if (!EntriesMatch(expected.baseVertices, [&](uint32_t i, auto& out) { return snapshot.BaseVertexAt(i, out); }) ||
        !EntriesMatch(expected.baseMaterials, [&](uint32_t i, auto& out) { return snapshot.BaseMaterialAt(i, out); })) return false;
    for (uint32_t family = 0; family < 5; ++family)
        if (snapshot.BaseUvCount(family) != expected.baseUv[family].size() ||
            !EntriesMatch(expected.baseUv[family], [&](uint32_t i, auto& out) { return snapshot.BaseUvAt(family, i, out); })) return false;
    return true;
}
std::vector<unsigned char> SourceBytes(const mdl::ModelRecord& model) {
    std::vector<unsigned char> bytes;
    const auto append = [&](const void* source, size_t size) {
        if (!size) return;
        Check(source != nullptr, "native morph table exists before source byte comparison");
        const auto* begin = static_cast<const unsigned char*>(source); bytes.insert(bytes.end(), begin, begin + size);
    };
    append(&model, sizeof(model)); append(model.morphs, model.morphCount * sizeof(mdl::MorphRecord));
    for (uint32_t i = 0; i < model.morphCount; ++i) {
        const auto& raw = model.morphs[i];
        if (model.physicsMode != 2 || raw.type == 1) append(raw.vertexEntries, size_t(raw.offsetCount) * sizeof(*raw.vertexEntries));
        else if (raw.type == 0) append(raw.groupEntries, size_t(raw.groupCount) * sizeof(*raw.groupEntries));
        else if (raw.type == 2) append(raw.boneEntries, size_t(raw.boneCount) * sizeof(*raw.boneEntries));
        else if (raw.type >= 3 && raw.type <= 7)
            append(raw.uvEntries[raw.type - 3], size_t(raw.uvCounts[raw.type - 3]) * sizeof(mdl::PmxUvMorphEntry));
        else if (raw.type == 8) append(raw.materialEntries, size_t(raw.materialCount) * sizeof(*raw.materialEntries));
    }
    append(model.morph0Table, model.morph0Count * sizeof(*model.morph0Table));
    for (uint32_t family = 0; family < 5; ++family)
        append(model.uvMorphTables.byFamily[family], size_t(model.uvMorphCounts.byFamily[family]) * sizeof(mdl::PmxUvMorphEntry));
    for (const auto* pool : {model.materialMorphPools.base, model.materialMorphPools.additive, model.materialMorphPools.multiplicative})
        if (pool) append(pool, model.materialCount * sizeof(*pool));
    if (model.boneMorphTable) append(model.boneMorphTable, size_t(model.boneMorphCount) * sizeof(*model.boneMorphTable));
    return bytes;
}
void Workers(const MorphDefinitionSnapshot& old, const Expected& original,
    const MorphDefinitionSnapshot& newer, const Expected& edited) {
    bool firstOk = true, secondOk = true;
    const auto read = [&](bool& ok) { for (unsigned i = 0; i < 128; ++i) ok = Matches(old, original) && Matches(newer, edited) && ok; };
    std::thread first([&] { read(firstOk); }); std::thread second;
    try { second = std::thread([&] { read(secondOk); }); } catch (...) { first.join(); throw; }
    first.join(); second.join();
    Check(firstOk && secondOk, "concurrent morph reads preserve all original and edited payloads");
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "real morph definition fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load morph definition fixture");
    auto& model = *mdl::Mdl(raw);
    Check(model.morphCount > 1 && model.morphs && model.morph0Count && model.morph0Table, "fixture has real morph and restore tables");
    const auto original = ReadExpected(model);
    uint32_t vertexMorph = 0;
    if (model.physicsMode == 2) {
        Check(original.morphs.size() == 9 && original.additionalUv == 4 && !original.baseMaterials.empty(),
            "PMX fixture includes every 2.0 morph type and material restore data");
        for (uint32_t type = 0; type < 9; ++type) Check(original.morphs[type].definition.type == type, "PMX fixture retains type order");
        Check(original.morphs[0].groups.size() == 2 && original.morphs[0].groups[0].morphIndex == 1 &&
            original.morphs[0].groups[1].morphIndex == 1 && original.morphs[0].groups[1].weight < 0,
            "duplicate and negative group entries are present");
        Check(!original.morphs[2].bones.empty() && !original.morphs[8].materials.empty() &&
            original.morphs[8].materials[0].materialIndex == -1 && original.morphs[8].materials[0].operation == 197,
            "bone offsets and all-material nonboolean operations are present");
        for (uint32_t family = 0; family < 5; ++family)
            Check(!original.morphs[family + 3].uv[family].empty() && !original.baseUv[family].empty(), "all UV families have offsets and restore values");
        vertexMorph = 1;
    } else {
        Check(original.morphs[1].definition.type == 0 && !original.morphs[1].vertices.empty() &&
            original.morphs[1].vertices[0].vertexIndex == 3 && original.morphs[1].groups.empty(),
            "PMD type zero is a vertex payload already remapped through the reordered base table");
        vertexMorph = 1;
    }
    app->CurrentFrame() = 3; app->SceneModified() = 0;
    const auto before = SourceBytes(model);
    auto old = MorphDefinitionSnapshot::Capture(raw);
    Check(old && Matches(*old, original), "snapshot matches direct native morph definitions and restore data");
    Check(before == SourceBytes(model) && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "capture preserves source records deep tables playback and modified flag");
    if (model.physicsMode == 2) {
        auto& type = model.morphs[vertexMorph].type;
        const auto savedType = type;
        for (const uint8_t unsupported : {uint8_t(9), uint8_t(10)}) {
            type = unsupported;
            bool rejected = false;
            try { auto invalid = MorphDefinitionSnapshot::Capture(raw); }
            catch (const std::invalid_argument&) { rejected = true; }
            catch (...) { type = savedType; throw; }
            type = savedType;
            Check(rejected && before == SourceBytes(model) && app->CurrentFrame() == 3 && app->SceneModified() == 0 &&
                Matches(*old, original), "opaque PMX types are rejected without editing data or an existing snapshot");
        }
    }
    auto& offset = model.morphs[vertexMorph].vertexEntries[0].offset[0];
    const float savedOffset = offset; offset += .375f;
    const auto edited = ReadExpected(model); const auto pending = SourceBytes(model);
    std::unique_ptr<MorphDefinitionSnapshot> newer;
    try { newer = MorphDefinitionSnapshot::Capture(raw); } catch (...) { offset = savedOffset; throw; }
    const bool unchanged = pending == SourceBytes(model); offset = savedOffset;
    Check(newer && unchanged && app->CurrentFrame() == 3 && app->SceneModified() == 0,
        "capture of an edited offset remains read-only");
    Check(!Matches(*old, edited) && !Matches(*newer, original), "snapshots retain distinct vertex definition revisions");
    Workers(*old, original, *newer, edited);
    Check(before == SourceBytes(model), "live definition restored before project save");
    const auto project = std::filesystem::absolute(L"morph-definition.pmm"); app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save actual morph fixture PMM");
    ResetAppState(app); Check(!app->ModelSlot(0), "destroy original morph definition source");
    Workers(*old, original, *newer, edited);
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0), "reload saved morph project"); Workers(*old, original, *newer, edited);
    auto& reloaded = *mdl::Mdl(app->ModelSlot(0));
    const auto reloadExpected = ReadExpected(reloaded); const auto reloadBefore = SourceBytes(reloaded);
    const auto reloadFrame = app->CurrentFrame(); const auto reloadModified = app->SceneModified();
    auto reloadSnapshot = MorphDefinitionSnapshot::Capture(app->ModelSlot(0));
    Check(reloadSnapshot && Matches(*reloadSnapshot, reloadExpected) && reloadBefore == SourceBytes(reloaded) &&
        app->CurrentFrame() == reloadFrame && app->SceneModified() == reloadModified, "reloaded definitions capture without changes");
    ResetAppState(app); Check(Matches(*reloadSnapshot, reloadExpected), "reloaded snapshot survives source deletion");
    std::puts("Frozen morph definitions: native payloads, restore tables, concurrent reads and independent lifetime passed");
}
}
#endif
