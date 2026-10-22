// Include after native_model_names_persistence_checks.inc.
// CPU metadata consumer checks: the adaptation context is explicit here.
// This does not exercise V2 workspaces, PMX loading, or the production app init.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace model_adaptation_names_checks {
using namespace plugins;
using NamesFixture = model_names_checks::Fixture;
using model_names_persistence_checks::Before;
using model_names_persistence_checks::CommentsKey;
using model_names_persistence_checks::NamesKey;
using model_names_persistence_checks::Registered;
using model_names_persistence_checks::Wire;

struct Bytes {
    struct Field { const void* address; std::vector<std::uint8_t> value; };
    std::vector<Field> fields;
    template<class T> void Add(const T& field) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(&field);
        fields.push_back({&field, {begin, begin + sizeof(field)}});
    }
    void Unchanged() const {
        for (const auto& field : fields)
            Check(!std::memcmp(field.address, field.value.data(), field.value.size()),
                "adaptation metadata leaves observed native bytes unchanged");
    }
};

struct Names {
    struct Text { wchar_t** field; wchar_t* pointer; std::wstring value; };
    struct Group { mdl::ModelRecord* model; mdl::DisplayGroup* field;
        text_encoding::DisplayGroupTextState state; std::uint8_t flags; };
    std::array<std::wstring, 2> model;
    std::vector<std::array<std::wstring, 2>> bones, morphs, groups;
    std::vector<std::uint8_t> flags;
    std::vector<Text> text;
    std::vector<Group> sidecars;
    Bytes raw;
    explicit Names(NamesFixture& f, unsigned slot) {
        auto& m = *mdl::Mdl(f.app->ModelSlot(slot));
        const auto remember = [&](wchar_t*& field) {
            text.push_back({&field, field, field ? field : L""});
        };
        for (unsigned language = 0; language < 2; ++language) {
            model[language] = text_encoding::ModelName(m, language != 0);
            remember(m.pmxTextBuffers[language]);
        }
        raw.Add(m.name); raw.Add(m.nameEn);
        for (unsigned i = 0; i < m.boneCount; ++i) {
            bones.push_back({text_encoding::BoneName(m.boneTable[i], false),
                text_encoding::BoneName(m.boneTable[i], true)});
            remember(m.boneTable[i].jpText); remember(m.boneTable[i].enText);
            raw.Add(m.boneTable[i].name); raw.Add(m.boneTable[i].nameEn);
        }
        for (unsigned i = 0; i < m.morphCount; ++i) {
            morphs.push_back({text_encoding::MorphName(m.morphs[i], false),
                text_encoding::MorphName(m.morphs[i], true)});
            remember(m.morphs[i].jpText); remember(m.morphs[i].enText);
            raw.Add(m.morphs[i].name); raw.Add(m.morphs[i].nameEn);
        }
        auto* table = mdl::DisplayGroups(f.app->ModelSlot(slot));
        for (unsigned i = 0; i < mdl::DisplayGroupCount(m); ++i) {
            groups.push_back({text_encoding::DisplayGroupName(table[i], false),
                text_encoding::DisplayGroupName(table[i], true)});
            flags.push_back(table[i].flags);
            text_encoding::DisplayGroupTextState state;
            Check(text_encoding::CaptureDisplayGroupText(m, table[i], state),
                "capture adaptation display-sidecar identity");
            sidecars.push_back({&m, &table[i], std::move(state), table[i].flags});
            raw.Add(table[i].name); raw.Add(table[i].nameEn);
        }
    }
    void Values(NamesFixture& f, unsigned slot) const {
        const auto& m = *mdl::Mdl(f.app->ModelSlot(slot));
        Check(bones.size() == m.boneCount && morphs.size() == m.morphCount &&
            groups.size() == mdl::DisplayGroupCount(m), "adaptation never changes native table counts");
        for (unsigned language = 0; language < 2; ++language)
            Check(text_encoding::ModelName(m, language != 0) == model[language],
                "adaptation checks each model-name language independently");
        for (unsigned i = 0; i < m.boneCount; ++i) for (unsigned language = 0; language < 2; ++language)
            Check(text_encoding::BoneName(m.boneTable[i], language != 0) == bones[i][language],
                "adaptation keeps the expected bone name at its exact index");
        for (unsigned i = 0; i < m.morphCount; ++i) for (unsigned language = 0; language < 2; ++language)
            Check(text_encoding::MorphName(m.morphs[i], language != 0) == morphs[i][language],
                "adaptation keeps the expected morph name at its exact index");
        const auto* table = mdl::DisplayGroups(f.app->ModelSlot(slot));
        for (unsigned i = 0; i < groups.size(); ++i) {
            for (unsigned language = 0; language < 2; ++language)
                Check(text_encoding::DisplayGroupName(table[i], language != 0) == groups[i][language],
                    "adaptation keeps both display-name languages at their exact index");
            Check(table[i].flags == flags[i], "adaptation keeps the expected display flag byte");
        }
    }
    void Unchanged(NamesFixture& f, unsigned slot) const {
        Values(f, slot); raw.Unchanged();
        for (const auto& field : text)
            Check(*field.field == field.pointer && (!field.pointer || std::wstring(field.pointer) == field.value),
                "ignored names keep the original owned-text pointer and contents");
        for (const auto& group : sidecars)
            Check(group.field->flags == group.flags &&
                text_encoding::MatchesDisplayGroupText(*group.model, *group.field, group.state),
                "ignored names keep the original display-sidecar node and flags");
    }
};

struct Activity {
    NamesFixture& f;
    Bytes bytes;
    std::uint64_t sceneRevision, generation, historyRevision = 0, retained, namesUiRevision;
    std::size_t undo, redo;
    unsigned commands;
    std::uint8_t sceneDirty, enhanced, physics, recorded;
    explicit Activity(NamesFixture& fixture) : f(fixture),
        sceneRevision(f.app->SceneRevision()), generation(f.app->SceneGeneration()),
        retained(f.history.RetainedBytes()), namesUiRevision(NativeModelNamesUiRevision()),
        undo(f.history.UndoCount()), redo(f.history.RedoCount()),
        commands(f.commands), sceneDirty(static_cast<const MMDApp&>(*f.app).SceneModified()),
        enhanced(f.app->EnhancedModelDirty()), physics(f.app->PhysicsResetPending()),
        recorded(f.app->RecordedFrameCountByte1()) {
        Check(f.history.Revision(historyRevision) == MMS_OK, "capture real common-history revision before metadata load");
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& m = *mdl::Mdl(f.app->ModelSlot(slot));
            bytes.Add(f.app->ModelSlot(slot)); bytes.Add(m.selectedBone); bytes.Add(m.frameRegistrationSelection);
            bytes.Add(m.lightDir);
            for (unsigned i = 0; i < m.boneCount; ++i) {
                bytes.Add(m.boneTable[i].trans); bytes.Add(m.boneTable[i].rotQuat);
                bytes.Add(m.boneSelection[i]); bytes.Add(m.bonePhysicsState[i]);
            }
            for (unsigned i = 0; i < m.morphCount; ++i) bytes.Add(m.morphKeys[i]);
            for (unsigned i = 0; i < m.boneCount; ++i) bytes.Add(m.boneKeys[i]);
            bytes.Add(m.displayKeys[0]);
        }
    }
    void Unchanged() const {
        std::uint64_t revision = 0;
        Check(f.history.Revision(revision) == MMS_OK && revision == historyRevision &&
            f.history.UndoCount() == undo && f.history.RedoCount() == redo && f.history.RetainedBytes() == retained &&
            NativeModelNamesUiRevision() == namesUiRevision,
            "metadata load never commits, clears, or advances common history");
        Check(f.app->SceneRevision() == sceneRevision && f.app->SceneGeneration() == generation &&
            static_cast<const MMDApp&>(*f.app).SceneModified() == sceneDirty &&
            f.app->EnhancedModelDirty() == enhanced && f.app->PhysicsResetPending() == physics &&
            f.app->RecordedFrameCountByte1() == recorded && f.commands == commands && f.Unmoved(),
            "metadata load does not change dirty flags, physics, playhead, selection or dispatch commands");
        bytes.Unchanged();
    }
};

std::wstring Label(const wchar_t* prefix, unsigned slot, const wchar_t* kind, unsigned index, unsigned language) {
    return std::wstring(prefix) + L"-slot-" + std::to_wstring(slot) + L"-" + kind + L"-" +
        std::to_wstring(index) + L"-language-" + std::to_wstring(language) + L"-\u540D\u79F0-" + std::wstring(24, L'X');
}
void Seed(NamesFixture& f, const wchar_t* prefix) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& m = *mdl::Mdl(f.app->ModelSlot(slot));
        for (unsigned language = 0; language < 2; ++language) {
            Check(extended_names::Assign(m.pmxTextBuffers[language], Label(prefix, slot, L"model", 0, language)),
                "seed distinguishable bilingual model names");
            Check(extended_names::Assign(m.pmxTextBuffers[2 + language], Label(prefix, slot, L"comment", 0, language)),
                "seed distinguishable independent comments");
        }
        for (unsigned i = 0; i < m.boneCount; ++i)
            Check(extended_names::Assign(m.boneTable[i].jpText, Label(prefix, slot, L"bone", i, 0)) &&
                extended_names::Assign(m.boneTable[i].enText, Label(prefix, slot, L"bone", i, 1)),
                "seed distinguishable bilingual bone names");
        for (unsigned i = 0; i < m.morphCount; ++i)
            Check(extended_names::Assign(m.morphs[i].jpText, Label(prefix, slot, L"morph", i, 0)) &&
                extended_names::Assign(m.morphs[i].enText, Label(prefix, slot, L"morph", i, 1)),
                "seed distinguishable bilingual morph names");
        auto* groups = mdl::DisplayGroups(f.app->ModelSlot(slot));
        for (unsigned i = 0; i < mdl::DisplayGroupCount(m); ++i) {
            const auto native = Label(prefix, slot, L"group", i, 0), english = Label(prefix, slot, L"group", i, 1);
            text_encoding::SetDisplayGroupName(m, groups[i], native.c_str(), english.c_str());
            groups[i].flags = static_cast<std::uint8_t>((prefix[0] == L's' ? 40 : 80) + slot * 8 + i);
        }
    }
}
pmm_extension::Document Document(NamesFixture& source) {
    pmm_extension::Document document;
    pmm_extension::Set(document, "org.example.adaptation.unknown", {3, 7, 11});
    extended_names::CapturePmm(*source.app, document);
    return Wire(document);
}
void RetainedHistory(NamesFixture& target) {
    auto first = target.Read(); first.model += L"-retained-history-model";
    model_names_checks::Commit(target, first);
    auto second = target.Read(); second.comment += L"-retained-history-comment";
    model_names_checks::Commit(target, second);
    Check(target.history.History(false) == MMS_OK && target.history.UndoCount() == 1 && target.history.RedoCount() == 1,
        "seed actual retained native history on both sides of the cursor before metadata preparation");
}
void Comments(NamesFixture& source, NamesFixture& target, unsigned slot, unsigned mask = 3) {
    const auto& saved = *mdl::Mdl(source.app->ModelSlot(slot));
    auto& loaded = *mdl::Mdl(target.app->ModelSlot(slot));
    for (unsigned language = 0; language < 2; ++language) if (mask & (1u << language))
        Registered(loaded.pmxTextBuffers[2 + language], saved.pmxTextBuffers[2 + language]);
}
std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    Check(at <= bytes.size() && bytes.size() - at >= 4, "locate a complete fixture u32");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[at + i]) << (8 * i);
    return value;
}
void WriteU32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    Check(at <= bytes.size() && bytes.size() - at >= 4, "replace only a complete fixture u32");
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void SkipText(const std::vector<std::uint8_t>& bytes, std::size_t& at) {
    const auto count = ReadU32(bytes, at); at += 4;
    Check(count <= bytes.size() - at, "locate a complete production UTF8 fixture field"); at += count;
}
std::array<std::size_t, 4> CountOffsets(const std::vector<std::uint8_t>& bytes) {
    Check(ReadU32(bytes, 0) == 1, "source name fixture retains the public v1 layout");
    std::size_t at = 4; SkipText(bytes, at); SkipText(bytes, at);
    std::array<std::size_t, 4> offsets{};
    offsets[0] = at; const auto bones = ReadU32(bytes, at); at += 4;
    for (std::uint32_t i = 0; i < bones; ++i) { SkipText(bytes, at); SkipText(bytes, at); }
    offsets[1] = at; const auto morphs = ReadU32(bytes, at); at += 4;
    for (std::uint32_t i = 0; i < morphs; ++i) { SkipText(bytes, at); SkipText(bytes, at); }
    offsets[2] = at; offsets[3] = at + 4;
    ReadU32(bytes, offsets[2]); ReadU32(bytes, offsets[3]); return offsets;
}
void Mismatch(pmm_extension::Document& document, unsigned which) {
    auto bytes = pmm_extension::Find(document, NamesKey(0))->value;
    const auto offsets = CountOffsets(bytes);
    WriteU32(bytes, offsets[which], ReadU32(bytes, offsets[which]) + 1);
    pmm_extension::Set(document, NamesKey(0), std::move(bytes));
}
void OneComment(pmm_extension::Document& document, unsigned slot, unsigned language) {
    const auto& original = pmm_extension::Find(document, CommentsKey(slot))->value;
    Check(ReadU32(original, 0) == 1 && ReadU32(original, 4) == 3, "source fixture contains both independent comments");
    std::size_t first = 8, second = first; SkipText(original, second);
    std::size_t end = second; SkipText(original, end);
    Check(end == original.size(), "source comment fixture has no trailing bytes");
    std::vector<std::uint8_t> bytes(original.begin(), original.begin() + 8);
    WriteU32(bytes, 4, 1u << language);
    bytes.insert(bytes.end(), original.begin() + (language ? second : first), original.begin() + (language ? end : second));
    pmm_extension::Set(document, CommentsKey(slot), std::move(bytes));
}
void ContextConsumer() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const Names saved0(source, 0), saved1(source, 1); const auto document = Document(source);
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::printf("CASE adaptation CPU explicit-context mode=%u\n", mode);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        auto& m = *mdl::Mdl(target.app->ModelSlot(0));
        if (mode == 0) {
            // Same counts, with deliberately exchanged names at the old indices.
            std::swap(m.boneTable[0].jpText, m.boneTable[1].jpText);
            std::swap(m.boneTable[0].enText, m.boneTable[1].enText);
            std::swap(m.morphs[0].jpText, m.morphs[1].jpText);
            std::swap(m.morphs[0].enText, m.morphs[1].enText);
        } else if (mode == 1) {
            Check(extended_names::Assign(m.boneTable[1].jpText, L"replacement-only native bone name") &&
                extended_names::Assign(m.morphs[1].enText, L"replacement-only English morph name"),
                "same-count replacement names are explicit context-consumer inputs");
        }
        const Names before0(target, 0), before1(target, 1); const Activity activity(target);
        const std::vector<int> adapted = mode == 2 ? std::vector<int>{1, 0} : std::vector<int>{0};
        extended_names::LoadPmm(*target.app, document, adapted);
        before0.Unchanged(target, 0);
        if (mode == 2) before1.Unchanged(target, 1); else saved1.Values(target, 1);
        Comments(source, target, 0); Comments(source, target, 1); activity.Unchanged();
        Check(pmm_extension::Find(document, "org.example.adaptation.unknown")->value == std::vector<std::uint8_t>({3, 7, 11}),
            "adaptation consumer never changes unrelated project records");
    }
    // The legacy overload is a deliberate control: no adaptation context means
    // both same-shaped dictionaries load. It does not detect source changes.
    NamesFixture control; NativePoseTestBinding binding(*control.app, control.history); Seed(control, L"target");
    const Activity activity(control); extended_names::LoadPmm(*control.app, document);
    saved0.Values(control, 0); saved1.Values(control, 1); activity.Unchanged();
}
void IndependentComments() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const Names saved1(source, 1); const auto original = Document(source);
    for (unsigned mode = 0; mode < 4; ++mode) {
        std::printf("CASE adaptation CPU independent-comments mode=%u\n", mode);
        auto document = original;
        if (mode < 2) OneComment(document, 0, mode);
        if (mode == 2) pmm_extension::Erase(document, CommentsKey(0));
        if (mode == 3) pmm_extension::Set(document, CommentsKey(0), {1, 0, 0, 0, 0, 0, 0, 0});
        document = Wire(document);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        auto& m = *mdl::Mdl(target.app->ModelSlot(0)); const Names before(target, 0); const Activity activity(target);
        const std::array<wchar_t*, 2> pointers{m.pmxTextBuffers[2], m.pmxTextBuffers[3]};
        const std::array<std::wstring, 2> text{pointers[0], pointers[1]};
        extended_names::LoadPmm(*target.app, document, {0});
        before.Unchanged(target, 0); saved1.Values(target, 1); Comments(source, target, 1);
        for (unsigned language = 0; language < 2; ++language) {
            if (mode == language) Comments(source, target, 0, 1u << language);
            else Check(m.pmxTextBuffers[2 + language] == pointers[language] &&
                std::wstring(pointers[language]) == text[language], "missing comment record or presence bit preserves that source-language allocation");
        }
        activity.Unchanged();
    }
}
void CountMismatches() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const Names saved1(source, 1); const auto original = Document(source);
    for (unsigned which = 0; which < 4; ++which) {
        std::printf("CASE adaptation CPU record-count-mismatch kind=%u\n", which);
        auto document = original; Mismatch(document, which); document = Wire(document);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        const Names before(target, 0); const Activity activity(target);
        // No adapted flag: a structural count mismatch independently ignores
        // the whole optional dictionary, without touching actual table sizes.
        extended_names::LoadPmm(*target.app, document);
        before.Unchanged(target, 0); saved1.Values(target, 1);
        Comments(source, target, 0); Comments(source, target, 1); activity.Unchanged();
    }
}
void IgnoredMalformedNames() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const Names saved1(source, 1); const auto original = Document(source);
    for (unsigned malformed = 0; malformed < 3; ++malformed) {
        std::printf("CASE adaptation CPU ignored-malformed-names kind=%u\n", malformed);
        auto document = original; auto value = pmm_extension::Find(document, NamesKey(0))->value;
        if (malformed == 0) value.clear();
        if (malformed == 1) value.pop_back();
        if (malformed == 2) value = {1, 0, 0, 0, 255, 255, 255, 255};
        pmm_extension::Set(document, NamesKey(0), std::move(value)); document = Wire(document);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        const Names before(target, 0); const Activity activity(target);
        extended_names::LoadPmm(*target.app, document, {0});
        before.Unchanged(target, 0); saved1.Values(target, 1);
        Comments(source, target, 0); Comments(source, target, 1); activity.Unchanged();
    }
}
void FailureAtomicity() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const auto original = Document(source);
    for (unsigned malformed = 0; malformed < 6; ++malformed) {
        std::printf("CASE adaptation CPU global-atomicity malformed=%u\n", malformed);
        auto document = original; const std::vector<int> adapted = malformed >= 4 ? std::vector<int>{} : std::vector<int>{0};
        const auto key = malformed == 2 ? NamesKey(1) : malformed == 5 ? NamesKey(0) : CommentsKey(malformed == 1 ? 1 : 0);
        auto value = pmm_extension::Find(document, key)->value;
        if (malformed == 0 || malformed == 2 || malformed == 4 || malformed == 5) value.pop_back();
        if (malformed == 1) value = {1, 0, 0, 0, 4, 0, 0, 0};
        if (malformed == 3) value = {1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0xc0, 0x80};
        if (malformed == 4) Mismatch(document, 2);
        pmm_extension::Set(document, key, std::move(value)); document = Wire(document);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        const Before before(*target.app); const Activity activity(target); bool rejected = false;
        try { extended_names::LoadPmm(*target.app, document, adapted); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "malformed applicable names or comments reject the complete cross-model metadata publication");
        before.Unchanged(); activity.Unchanged();
    }
}
void AllocationAtomicity() {
    NamesFixture source; NativePoseTestBinding sourceBinding(*source.app, source.history); Seed(source, L"saved");
    const Names saved0(source, 0), saved1(source, 1); const auto document = Document(source);
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::printf("CASE adaptation CPU allocation-atomicity mode=%u\n", mode);
        NamesFixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Seed(target, L"target");
        RetainedHistory(target);
        const Names before0(target, 0), before1(target, 1); const Before before(*target.app); const Activity activity(target);
        const std::vector<int> adapted = mode == 0 ? std::vector<int>{} : mode == 1 ? std::vector<int>{0} : std::vector<int>{0, 1};
        bool completed = false; unsigned failures = 0; long completedAt = -1;
        for (long limit = 0; limit < 4096 && !completed; ++limit) {
            failAfter = limit;
            try { extended_names::LoadPmm(*target.app, document, adapted); completed = true; }
            catch (const std::bad_alloc&) { ++failures; }
            catch (...) { failAfter = -1; throw; }
            failAfter = -1;
            if (!completed) { before.Unchanged(); activity.Unchanged(); }
            else completedAt = limit;
        }
        std::printf("DIAG adaptation allocation mode=%u failures=%u successLimit=%ld\n", mode, failures, completedAt);
        Check(completed && failures > 0 && completedAt > 0,
            "allocation sweep reaches an actual failure and one complete metadata publication");
        if (mode == 0) saved0.Values(target, 0); else before0.Unchanged(target, 0);
        if (mode == 2) before1.Unchanged(target, 1); else saved1.Values(target, 1);
        Comments(source, target, 0); Comments(source, target, 1); activity.Unchanged();
    }
}
} // namespace model_adaptation_names_checks
} // namespace

void NativeModelAdaptationNamesChecks() {
    model_adaptation_names_checks::ContextConsumer();
    model_adaptation_names_checks::IndependentComments();
    model_adaptation_names_checks::CountMismatches();
    model_adaptation_names_checks::IgnoredMalformedNames();
    model_adaptation_names_checks::FailureAtomicity();
    model_adaptation_names_checks::AllocationAtomicity();
}
