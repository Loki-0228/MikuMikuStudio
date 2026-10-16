#include "scene_identity.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_layout.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/build_profile.hpp"
#include <set>

namespace mikudancestudio::plugins {
namespace {
constexpr char identityKey[] = "org.mikudancestudio.plugin.objectIdentity.v1";
SceneIdentity identities;
uint64_t revision = 1;
std::vector<unsigned char> previous;
bool opaqueIdentity = false;
void Identity(float* matrix) { std::fill(matrix, matrix + 16, 0.f); matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1; }
template<class T> void Append(std::vector<unsigned char>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "identity scalar");
    const auto* first = reinterpret_cast<const unsigned char*>(&value); bytes.insert(bytes.end(), first, first + sizeof(value));
}
void Append(std::vector<unsigned char>& bytes, MmsString text) {
    Append(bytes, text.bytes); bytes.insert(bytes.end(), text.data, text.data + text.bytes);
}
struct Reader {
    const std::vector<uint8_t>& bytes; size_t offset = 0;
    template<class T> T Get() {
        if (bytes.size() - offset < sizeof(T)) throw std::invalid_argument("truncated identity record");
        T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); offset += sizeof(value); return value;
    }
};
const void* Token(const MMDApp& app, uint32_t kind, uint32_t slot) {
    if (slot >= 255 || (kind != MMS_OBJECT_MODEL && kind != MMS_OBJECT_ACCESSORY)) throw std::invalid_argument("identity object slot");
    return kind == MMS_OBJECT_MODEL ? static_cast<const void*>(app.ModelSlot(slot)) : app.AccessorySlot(slot);
}
void EnsureBones(const void* token, uint32_t kind) {
    identities.Get(token);
    if (kind == MMS_OBJECT_MODEL) {
        const auto* model = mdl::Mdl(static_cast<const unsigned char*>(token));
        for (uint32_t i = 0; i < model->boneCount; ++i) identities.Element(token, MMS_CHANNEL_BONES, i);
    }
}
MmsResult Capture(MMDApp* app, const MmsSceneQueryV1& query, std::shared_ptr<const SceneData>& out) {
    if (!app || app->SceneMutationInProgress() || !app->WindowLayoutReady()) return MMS_E_NOT_READY;
    // Each additional stage is published separately after its actual pipeline
    // boundary is captured. Bind data never substitutes for an evaluated pose.
    if (query.stage != MMS_STAGE_BIND || (query.channels & ~uint64_t(MMS_CHANNEL_BONES))) return MMS_E_UNSUPPORTED;
    auto data = std::make_shared<SceneData>();
    data->time = Normalize({app->CurrentFrame(), 30}); data->supported = MMS_CHANNEL_BONES;
    std::vector<unsigned char> fingerprint;
    for (uint32_t kind : {MMS_OBJECT_MODEL, MMS_OBJECT_ACCESSORY}) for (uint32_t slot = 0; slot < 255; ++slot) {
        const void* token = Token(*app, kind, slot); if (!token) continue;
        EnsureBones(token, kind);
        SceneData::Channels record;
        auto& object = record.object; object.h = Header<MmsObjectV1>(); object.object = identities.Get(token).object;
        object.kind = kind; Identity(object.world);
        std::vector<MmsBoneV1> bones;
        if (kind == MMS_OBJECT_MODEL) {
            const auto& model = *mdl::Mdl(static_cast<const unsigned char*>(token));
            object.name = data->Keep(Utf8(text_encoding::ModelName(model, false)));
            object.sourcePath = data->Keep(Utf8(model.path)); object.visible = model.loadComplete != 0;
            if (model.boneCount && !model.boneTable) return MMS_E_NOT_READY;
            bones.resize(model.boneCount);
            for (uint32_t i = 0; i < model.boneCount; ++i) {
                const auto& source = model.boneTable[i]; auto& bone = bones[i]; bone.h = Header<MmsBoneV1>();
                bone.id = bone.sourceId = identities.Element(token, MMS_CHANNEL_BONES, i);
                bone.name = data->Keep(Utf8(text_encoding::BoneName(source, false))); bone.flags = source.flags;
                for (auto* matrix : {bone.local, bone.world, bone.bindWorld, bone.inverseBindWorld}) Identity(matrix);
                const bool parent = source.parent >= 0 && uint32_t(source.parent) < model.boneCount;
                if (parent) bone.parentId = identities.Element(token, MMS_CHANNEL_BONES, uint32_t(source.parent));
                for (unsigned axis = 0; axis < 3; ++axis) {
                    bone.world[12 + axis] = bone.bindWorld[12 + axis] = source.position[axis];
                    bone.inverseBindWorld[12 + axis] = -source.position[axis];
                    bone.local[12 + axis] = source.position[axis] - (parent ? model.boneTable[source.parent].position[axis] : 0);
                }
                Append(fingerprint, bone.id); Append(fingerprint, bone.parentId); Append(fingerprint, bone.name);
                Append(fingerprint, bone.flags); Append(fingerprint, bone.bindWorld);
            }
        } else {
            const auto& accessory = *static_cast<const mdl::AccessoryRecord*>(token);
            object.sourcePath = data->Keep(Utf8(accessory.sourcePath));
            object.name = data->Keep(Utf8(fs::path(accessory.sourcePath).stem().native())); object.visible = accessory.visible;
            // Stage zero is source binding space. Scene attachment transforms
            // belong to the evaluated stages and are not read from live bones.
        }
        Append(fingerprint, object.object); Append(fingerprint, kind); Append(fingerprint, object.visible);
        Append(fingerprint, object.name); Append(fingerprint, object.sourcePath);
        record.values.emplace(MMS_CHANNEL_BONES, data->Keep(std::move(bones)));
        data->objects.push_back(std::move(record));
    }
    if (fingerprint != previous) { ++revision; previous = std::move(fingerprint); }
    data->revision = data->structure = revision; data->pose = 1;
    for (auto& record : data->objects) {
        record.object.structureRevision = revision; record.object.meshRevision = revision;
        record.object.materialRevision = revision; record.object.poseRevision = 1;
    }
    out = std::move(data); return MMS_OK;
}
}
std::shared_ptr<SceneStore> CreateSceneStore(MMDApp* app) {
    return std::make_shared<SceneStore>([app](const auto& query, auto& out) { return Capture(app, query, out); },
        [](MmsId id) { return identities.Known(id); });
}
void ForgetSceneObject(const void* token) { if (!build_profile::original) { identities.Forget(token); ++revision; previous.clear(); } }
void ResetSceneIdentity() { identities.Clear(); opaqueIdentity = false; previous.clear(); ++revision; }
void SaveSceneIdentity(const MMDApp& app, pmm_extension::Document& document) {
    if (build_profile::original || opaqueIdentity) return;
    std::vector<uint8_t> bytes;
    Append(bytes, uint32_t(1)); const size_t countOffset = bytes.size(); Append(bytes, uint32_t(0)); uint32_t count = 0;
    for (uint32_t kind : {MMS_OBJECT_MODEL, MMS_OBJECT_ACCESSORY}) for (uint32_t slot = 0; slot < 255; ++slot) {
        const auto* token = Token(app, kind, slot); if (!token) continue;
        EnsureBones(token, kind); const auto& entry = identities.Get(token);
        Append(bytes, kind); Append(bytes, slot); Append(bytes, entry.object.id.lo); Append(bytes, entry.object.id.hi);
        Append(bytes, uint32_t(entry.elements.size()));
        for (const auto& element : entry.elements) { Append(bytes, element.first); Append(bytes, element.second.lo); Append(bytes, element.second.hi); }
        ++count;
    }
    std::memcpy(bytes.data() + countOffset, &count, sizeof(count)); pmm_extension::Set(document, identityKey, std::move(bytes));
}
void LoadSceneIdentity(MMDApp& app, const pmm_extension::Document& document) {
    if (build_profile::original) return;
    const auto* record = pmm_extension::Find(document, identityKey); if (!record) return;
    auto result = Boundary([&]() -> MmsResult {
        if (record->value.size() > 64 * 1024 * 1024) return MMS_E_ARGUMENT;
        Reader reader{record->value}; if (reader.Get<uint32_t>() != 1) return MMS_E_VERSION;
        const auto count = reader.Get<uint32_t>(); if (count > 510) return MMS_E_ARGUMENT;
        std::vector<SceneIdentity::Restore> restored; std::set<uint64_t> slots; std::set<MmsId, IdLess> ids;
        for (uint32_t i = 0; i < count; ++i) {
            const auto kind = reader.Get<uint32_t>(), slot = reader.Get<uint32_t>();
            const auto* token = Token(app, kind, slot);
            if (!slots.insert(uint64_t(kind) << 32 | slot).second) return MMS_E_ARGUMENT;
            SceneIdentity::Entry entry;
            entry.object.id = {reader.Get<uint64_t>(), reader.Get<uint64_t>()};
            if (Empty(entry.object.id) || !ids.insert(entry.object.id).second) return MMS_E_ARGUMENT;
            const auto elements = reader.Get<uint32_t>(); if (elements > 1024 * 1024) return MMS_E_ARGUMENT;
            for (uint32_t n = 0; n < elements; ++n) {
                auto key = reader.Get<uint64_t>(); MmsId id{reader.Get<uint64_t>(), reader.Get<uint64_t>()};
                if (Empty(id) || !ids.insert(id).second || !entry.elements.emplace(key, id).second) return MMS_E_ARGUMENT;
            }
            if (token) restored.push_back({token, std::move(entry)});
        }
        if (reader.offset != record->value.size()) return MMS_E_ARGUMENT;
        identities.Replace(restored); previous.clear(); ++revision; return MMS_OK;
    });
    if (result) { opaqueIdentity = true; runtime_log::Write("PLUGIN_ABI identity record retained without applying: %d", result); }
}
}
