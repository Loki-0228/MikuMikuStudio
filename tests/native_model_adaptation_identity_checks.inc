// Include after native_model_names_persistence_checks.inc. Uses real process
// SceneIdentity save/load APIs and a real names fixture, without a PMM host,
// renderer, mocked Replace, or file-backed scene replacement.
#include "mikumikustudio/plugin_scene.hpp"
#include "../src/plugins/scene_identity.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace {
namespace model_adaptation_identity_checks {
using namespace plugins;
using NamesFixture = model_names_checks::Fixture;
using model_names_persistence_checks::Before;
using model_names_persistence_checks::Wire;
constexpr char kIdentityKey[] = "org.mikumikustudio.plugin.objectIdentity.v1";
constexpr char kUnrelatedKey[] = "org.example.adaptation.identity.sentinel";

struct IdentityScope {
    IdentityScope() { ResetSceneIdentity(); }
    ~IdentityScope() { ResetSceneIdentity(); }
    IdentityScope(const IdentityScope&) = delete;
    IdentityScope& operator=(const IdentityScope&) = delete;
};
struct NativeState {
    struct Field { const void* address; std::vector<std::uint8_t> bytes; };
    NamesFixture& f;
    std::vector<Field> fields;
    std::array<MmsObjectRef, 4> objects{};
    std::uint64_t revision = 0, retained, sceneRevision, sceneGeneration, namesUi;
    std::size_t undo, redo;
    unsigned commands;
    std::uint8_t dirty, enhanced, physics, recorded;
    template<class T> void Add(const T& value) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
        fields.push_back({&value, {begin, begin + sizeof(value)}});
    }
    explicit NativeState(NamesFixture& fixture) : f(fixture), retained(f.history.RetainedBytes()),
        sceneRevision(f.app->SceneRevision()), sceneGeneration(f.app->SceneGeneration()),
        namesUi(NativeModelNamesUiRevision()), undo(f.history.UndoCount()), redo(f.history.RedoCount()),
        commands(f.commands), dirty(static_cast<const MMDApp&>(*f.app).SceneModified()),
        enhanced(f.app->EnhancedModelDirty()), physics(f.app->PhysicsResetPending()),
        recorded(f.app->RecordedFrameCountByte1()) {
        Check(f.history.Revision(revision) == MMS_OK, "capture common history before identity loading");
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& model = *mdl::Mdl(f.app->ModelSlot(slot));
            Check(FindSceneObjectIdentity(f.app->ModelSlot(slot), objects[slot]) &&
                FindSceneObjectIdentity(f.app->AccessorySlot(slot), objects[2 + slot]),
                "the real saved fixture has live identities for both models and accessories");
            Add(f.app->ModelSlot(slot)); Add(model.path); Add(model.selectedBone); Add(model.frameRegistrationSelection);
            Add(model.lightDir);
            for (unsigned i = 0; i < model.boneCount; ++i) {
                Add(model.boneTable[i].trans); Add(model.boneTable[i].rotQuat);
                Add(model.boneSelection[i]); Add(model.bonePhysicsState[i]); Add(model.boneKeys[i]);
            }
            for (unsigned i = 0; i < model.morphCount; ++i) Add(model.morphKeys[i]);
            Add(model.displayKeys[0]);
        }
    }
    void SideEffectsUnchanged() const {
        std::uint64_t current = 0;
        Check(f.history.Revision(current) == MMS_OK && current == revision && f.history.UndoCount() == undo &&
            f.history.RedoCount() == redo && f.history.RetainedBytes() == retained && NativeModelNamesUiRevision() == namesUi,
            "identity load never records, clears or advances common history or name-edit UI state");
        Check(f.app->SceneRevision() == sceneRevision && f.app->SceneGeneration() == sceneGeneration &&
            static_cast<const MMDApp&>(*f.app).SceneModified() == dirty && f.app->EnhancedModelDirty() == enhanced &&
            f.app->PhysicsResetPending() == physics && f.app->RecordedFrameCountByte1() == recorded &&
            f.commands == commands && f.Stable(), "identity load leaves pose, playhead, dirty flags, physics and selection unchanged");
        for (const auto& field : fields) Check(!std::memcmp(field.address, field.bytes.data(), field.bytes.size()),
            "identity load leaves original model paths, native poses and key payloads byte-exact");
    }
    void ObjectRefsUnchanged() const {
        for (unsigned slot = 0; slot < 2; ++slot) {
            MmsObjectRef model{}, accessory{};
            Check(FindSceneObjectIdentity(f.app->ModelSlot(slot), model) && Same(model, objects[slot]) &&
                FindSceneObjectIdentity(f.app->AccessorySlot(slot), accessory) && Same(accessory, objects[2 + slot]),
                "refused identity load preserves every current object ID and runtime generation");
        }
    }
};

void SameDocument(const pmm_extension::Document& actual, const pmm_extension::Document& expected) {
    Check(actual.records.size() == expected.records.size(), "identity load does not add or remove caller records");
    for (std::size_t i = 0; i < actual.records.size(); ++i)
        Check(actual.records[i].key == expected.records[i].key && actual.records[i].value == expected.records[i].value,
            "identity load leaves every caller record key and payload byte unchanged");
}
pmm_extension::Document Saved(NamesFixture& f) {
    // Always start without an identity record. A prefilled document would hide
    // accidental opaque mode because the saver deliberately leaves it alone.
    pmm_extension::Document document;
    pmm_extension::Set(document, kUnrelatedKey, {17, 31, 63});
    SaveSceneIdentity(*f.app, document);
    return document;
}
const std::vector<std::uint8_t>& Payload(const pmm_extension::Document& document) {
    const auto* record = pmm_extension::Find(document, kIdentityKey);
    Check(record != nullptr, "fresh SaveSceneIdentity emits a real record instead of silently remaining opaque");
    return record->value;
}
struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at = 0;
    void Need(std::size_t count) const {
        Check(at <= bytes.size() && count <= bytes.size() - at, "real identity wire fields stay inside their payload");
    }
    std::uint32_t U32() {
        Need(4); std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[at++]) << (8 * i);
        return value;
    }
    std::uint64_t U64() {
        const auto low = U32(); return low | (std::uint64_t(U32()) << 32);
    }
    MmsId Id() { const auto low = U64(); return {low, U64()}; }
};
using ObjectKey = std::pair<std::uint32_t, std::uint32_t>;
struct Entry { MmsId id{}; std::map<std::uint64_t, MmsId> elements; };
using Entries = std::map<ObjectKey, Entry>;
Entries Parse(const pmm_extension::Document& document) {
    Reader reader{Payload(document)}; Entries result;
    Check(reader.U32() == 1, "the real identity saver emits the supported v1 record");
    const auto count = reader.U32(); Check(count == 4, "the real fixture saves two model and two accessory identities");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto kind = reader.U32(), slot = reader.U32(); Entry entry; entry.id = reader.Id();
        Check(!Empty(entry.id), "every actual saved object identity is nonempty");
        const auto elements = reader.U32();
        Check(elements <= (reader.bytes.size() - reader.at) / 24, "the actual element count fits its serialized rows");
        for (std::uint32_t n = 0; n < elements; ++n) {
            const auto key = reader.U64(); const auto id = reader.Id();
            Check(!Empty(id) && entry.elements.emplace(key, id).second, "the actual saved element IDs have unique positional keys");
        }
        Check(result.emplace(ObjectKey{kind, slot}, std::move(entry)).second, "the actual saved object kind and slot are unique");
    }
    Check(reader.at == reader.bytes.size(), "the identity wire reader consumes the complete actual payload");
    return result;
}
const Entry& Model(const Entries& entries, unsigned slot) {
    const auto found = entries.find({MMS_OBJECT_MODEL, slot});
    Check(found != entries.end(), "the serialized real fixture contains the selected model slot"); return found->second;
}
void SameEntry(const Entry& actual, const Entry& expected) {
    Check(Same(actual.id, expected.id) && actual.elements.size() == expected.elements.size(),
        "an unadapted object preserves its complete serialized identity set");
    for (const auto& element : expected.elements) {
        const auto found = actual.elements.find(element.first);
        Check(found != actual.elements.end() && Same(found->second, element.second),
            "an unadapted object preserves every element ID at its original key");
    }
}
void AdaptedElements(const Entries& actual, const Entries& before) {
    const auto& changed = Model(actual, 0); const auto& original = Model(before, 0);
    Check(Same(changed.id, original.id) && !changed.elements.empty() && changed.elements.size() == original.elements.size(),
        "adaptation keeps the model object ID while recreating its real bone element set");
    for (const auto& element : changed.elements) {
        Check((element.first >> 32) == MMS_CHANNEL_BONES,
            "the SaveSceneIdentity fixture observes actual bone elements rather than synthetic test IDs");
        for (const auto& retired : original.elements) Check(!Same(element.second, retired.second),
            "an adapted bone never revives any old positional element ID");
    }
    for (const auto& entry : before) if (entry.first != ObjectKey{MMS_OBJECT_MODEL, 0}) {
        const auto found = actual.find(entry.first); Check(found != actual.end(), "all other real objects remain serialized");
        SameEntry(found->second, entry.second);
    }
}
void WriteVersion(std::vector<std::uint8_t>& bytes, std::uint32_t version) {
    Check(bytes.size() >= 4, "the real identity record has a complete version field");
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(version >> (8 * i));
}

void RetiredIdentityAllocationChecks() {
    const auto replace = [](unsigned retiredCount, bool sweep) {
        SceneIdentity identities, importedSource; int retired = 0, token = 0, imported = 0;
        std::vector<MmsId> retiredIds; retiredIds.reserve(retiredCount);
        for (unsigned i = 0; i < retiredCount; ++i) {
            retiredIds.push_back(identities.Get(&retired).object.id);
            identities.Forget(&retired);
        }
        const auto oldBone = identities.Element(&token, MMS_CHANNEL_BONES, 0);
        const auto oldVertex = identities.Element(&token, MMS_CHANNEL_MESH, 2);
        const auto original = identities.Get(&token);
        const auto importedEntry = importedSource.Get(&imported);
        const std::vector<SceneIdentity::Restore> restore{{&token, original, false}, {&imported, importedEntry}};
        Check(!identities.Known(oldBone) && !identities.Known(oldVertex) && !identities.Known(importedEntry.object.id),
            "adaptation fixture starts with unretired live elements and an incoming unknown object ID");
        unsigned refusals = 0; long allocations = -1;
        // Two live elements need only bounded local staging, regardless of
        // process-lifetime retired IDs. This is deliberately not a cap grown
        // to accommodate copying the accumulated registry.
        for (long limit = sweep ? 0 : 64; limit <= 64; ++limit) {
            bool memory = false; long remaining = -1; failAfter = limit;
            try { identities.Replace(restore); remaining = failAfter; }
            catch (const std::bad_alloc&) { memory = true; }
            catch (...) { failAfter = -1; Check(false, "retired identity preparation has no unexpected exception"); }
            failAfter = -1;
            if (!memory) { allocations = limit - remaining; break; }
            ++refusals;
            const auto* current = identities.Retained(&token);
            Check(current && Same(current->object, original.object) && current->present && current->published &&
                current->elements.size() == original.elements.size() &&
                std::equal(current->elements.begin(), current->elements.end(), original.elements.begin(),
                    [](const auto& a, const auto& b) { return a.first == b.first && Same(a.second, b.second); }),
                "every pending-node allocation refusal leaves live identity generation and elements unchanged");
            Check(!identities.Known(oldBone) && !identities.Known(oldVertex) &&
                !identities.Known(importedEntry.object.id) && !identities.Retained(&imported),
                "failed adaptation never partially publishes retired elements or the incoming object identity");
            for (auto id : retiredIds) Check(identities.Known(id),
                "failed adaptation retains all previously retired object IDs");
        }
        Check(allocations >= 0 && allocations <= 64 && (!sweep || refusals > 0),
            "replacement of two elements succeeds within a small budget even after thousands of retired objects");
        const auto& current = identities.Get(&token);
        Check(Same(current.object.id, original.object.id) && current.object.generation == original.object.generation + 1 &&
            current.elements.empty() && identities.Known(oldBone) && identities.Known(oldVertex),
            "successful adaptation atomically retires elements and advances generation exactly once after refusals");
        Check(identities.Known(importedEntry.object.id) && Same(identities.Get(&imported).object.id, importedEntry.object.id) &&
            identities.Get(&imported).object.generation == original.object.generation + 2,
            "the same publication installs its incoming object and reserves the next runtime generation");
        for (auto id : retiredIds) Check(identities.Known(id), "successful adaptation preserves every historical tombstone");
        const auto newBone = identities.Element(&token, MMS_CHANNEL_BONES, 0);
        const auto newVertex = identities.Element(&token, MMS_CHANNEL_MESH, 2);
        Check(!Same(newBone, oldBone) && !Same(newBone, oldVertex) &&
            !Same(newVertex, oldBone) && !Same(newVertex, oldVertex),
            "adapted elements cannot reuse either retired positional identity");
        const auto reusedAddress = identities.Get(&retired).object.id;
        for (auto id : retiredIds) Check(!Same(reusedAddress, id), "recycled object addresses cannot reuse a retired ID");
        return allocations;
    };
    const auto fresh = replace(0, false), aged = replace(8192, true);
    Check(fresh == aged, "replacement allocation count depends on incoming IDs rather than historical registry size");
}

void AdaptedAllocationSweep() {
    IdentityScope identityScope; NamesFixture f; NativePoseTestBinding binding(*f.app, f.history);
    const auto saved = Saved(f); const auto original = Parse(saved); const auto expectedPayload = Payload(saved);
    auto document = Wire(saved); const auto input = document;
    const std::vector<int> adapted{0}; // Allocate the argument before arming failAfter.
    const Before before(*f.app); const NativeState state(f);
    Check(Model(original, 0).elements.size() == mdl::Mdl(f.app->ModelSlot(0))->boneCount &&
        Model(original, 1).elements.size() == mdl::Mdl(f.app->ModelSlot(1))->boneCount,
        "the valid production record starts with all actual bone IDs in both model slots");
    bool completed = false; unsigned failures = 0; long completedAt = -1;
    for (long limit = 0; limit < 8192 && !completed; ++limit) {
        bool memory = false; failAfter = limit;
        try { LoadSceneIdentity(*f.app, document, adapted); completed = true; }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { failAfter = -1; Check(false, "valid adapted identity allocation refusal must be std::bad_alloc"); }
        failAfter = -1;
        SameDocument(document, input); before.Unchanged(); state.SideEffectsUnchanged();
        if (memory) {
            ++failures; state.ObjectRefsUnchanged();
            const auto current = Saved(f);
            Check(Payload(current) == expectedPayload,
                "every failed adapted allocation preserves all serialized elements and leaves normal saving enabled");
        } else {
            Check(completed, "each valid adapted load attempt either throws memory failure or completes"); completedAt = limit;
        }
    }
    std::printf("DIAG adapted identity OOM failures=%u successLimit=%ld\n", failures, completedAt);
    Check(completed && failures > 0 && completedAt > 0, "the actual adapted identity allocation sweep reaches both refusal and success");
    const auto adaptedSave = Saved(f); AdaptedElements(Parse(adaptedSave), original);
    for (unsigned slot = 0; slot < 2; ++slot) {
        MmsObjectRef current{}; Check(FindSceneObjectIdentity(f.app->ModelSlot(slot), current) &&
            Same(current.id, state.objects[slot].id) && current.generation != state.objects[slot].generation,
            "successful identity replacement preserves each model ID and advances only its runtime generation");
    }
    const auto adaptedPayload = Payload(adaptedSave); auto reopened = Wire(adaptedSave); const auto reopenedInput = reopened;
    LoadSceneIdentity(*f.app, reopened); // Workspace now matches: no adapted context.
    const auto reopenedSave = Saved(f);
    Check(Payload(reopenedSave) == adaptedPayload,
        "saving the adapted IDs then reopening without context preserves the new IDs byte-for-byte");
    AdaptedElements(Parse(reopenedSave), original); SameDocument(reopened, reopenedInput);
    before.Unchanged(); state.SideEffectsUnchanged();
    std::puts("PASS actual identity adaptation OOM and save/reopen without old element resurrection");
}
void OrdinaryMemoryStillOpaque() {
    IdentityScope identityScope; NamesFixture f; NativePoseTestBinding binding(*f.app, f.history);
    const auto saved = Saved(f); auto document = Wire(saved); const auto input = document;
    const Before before(*f.app); const NativeState state(f);
    bool reachedOpaque = false, beforeBoundary = false;
    for (long budget = 0; budget < 8192 && !reachedOpaque; ++budget) {
        bool memory = false; failAfter = budget;
        try { LoadSceneIdentity(*f.app, document); }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { failAfter = -1; Check(false, "ordinary identity allocation refusal has no unexpected exception"); }
        failAfter = -1;
        state.ObjectRefsUnchanged(); before.Unchanged(); state.SideEffectsUnchanged(); SameDocument(document, input);
        if (memory) {
            // Find's long std::string key is allocated before Boundary. That
            // existing refusal propagates without setting opaque mode.
            beforeBoundary = true;
            Check(Payload(Saved(f)) == Payload(saved), "ordinary pre-boundary OOM preserves normal identity saving");
        } else {
            reachedOpaque = true;
            Check(!pmm_extension::Find(Saved(f), kIdentityKey), "ordinary in-boundary OOM still suppresses fresh identity capture in opaque mode");
        }
    }
    Check(reachedOpaque && beforeBoundary, "ordinary allocation sweep distinguishes pre-boundary failure from the retained opaque fallback");
    auto retained = document; SaveSceneIdentity(*f.app, retained); SameDocument(retained, document);
    std::puts("PASS ordinary no-adaptation OOM retains its prior opaque behavior");
}
void InvalidAndFutureRemainOpaque() {
    for (unsigned route = 0; route < 4; ++route) {
        std::printf("CASE identity malformed/future opaque route=%u\n", route);
        IdentityScope identityScope; NamesFixture f; NativePoseTestBinding binding(*f.app, f.history);
        auto document = Saved(f); auto value = Payload(document);
        if (route & 1u) WriteVersion(value, 2); else value.pop_back();
        pmm_extension::Set(document, kIdentityKey, std::move(value)); document = Wire(document); const auto input = document;
        const Before before(*f.app); const NativeState state(f); const std::vector<int> adapted = route >= 2 ? std::vector<int>{0} : std::vector<int>{};
        bool threw = false; try { LoadSceneIdentity(*f.app, document, adapted); } catch (...) { threw = true; }
        Check(!threw, "bad or future identity formats keep their old opaque behavior even with adaptation context");
        state.ObjectRefsUnchanged(); before.Unchanged(); state.SideEffectsUnchanged(); SameDocument(document, input);
        Check(!pmm_extension::Find(Saved(f), kIdentityKey), "bad or unknown identity records still prevent replacement by freshly generated IDs");
        auto retained = document; SaveSceneIdentity(*f.app, retained); SameDocument(retained, input);
    }
}
void MissingRecordRemainsNormal() {
    IdentityScope identityScope; NamesFixture f; NativePoseTestBinding binding(*f.app, f.history);
    const auto baseline = Saved(f); const auto expected = Payload(baseline);
    pmm_extension::Document empty; pmm_extension::Set(empty, kUnrelatedKey, {5, 9});
    const auto input = empty; const Before before(*f.app); const NativeState state(f);
    LoadSceneIdentity(*f.app, empty, {0});
    state.ObjectRefsUnchanged(); before.Unchanged(); state.SideEffectsUnchanged(); SameDocument(empty, input);
    Check(Payload(Saved(f)) == expected, "a missing identity record remains a normal no-op rather than entering opaque mode");
}
} // namespace model_adaptation_identity_checks
} // namespace

void NativeModelAdaptationIdentityChecks() {
    model_adaptation_identity_checks::RetiredIdentityAllocationChecks();
    model_adaptation_identity_checks::AdaptedAllocationSweep();
    model_adaptation_identity_checks::OrdinaryMemoryStillOpaque();
    model_adaptation_identity_checks::InvalidAndFutureRemainOpaque();
    model_adaptation_identity_checks::MissingRecordRemainsNormal();
}
