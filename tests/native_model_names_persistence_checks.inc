// Include after native_model_names_checks.inc. Uses its complete model fixture
// and the production MMS PMM trailer codec; no renderer or source file reload.
#include "mikumikustudio/save_compatibility.hpp"
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
namespace model_names_persistence_checks {
using namespace plugins;
std::string NamesKey(unsigned slot) { return "org.mikumikustudio.model." + std::to_string(slot) + ".unicode.v1"; }
std::string CommentsKey(unsigned slot) { return "org.mikumikustudio.model." + std::to_string(slot) + ".comments.v1"; }
struct WireFile {
    wchar_t path[MAX_PATH]{};
    int fd = -1;
    WireFile() {
        wchar_t directory[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"mnp", 0, path), "create private name-extension codec fixture");
        fd = _wopen(path, _O_BINARY | _O_RDWR); Check(fd >= 0, "open private name-extension codec fixture");
    }
    ~WireFile() { if (fd >= 0) _close(fd); if (*path) DeleteFileW(path); }
};
pmm_extension::Document Wire(const pmm_extension::Document& document) {
    WireFile file; std::array<unsigned char, 64> prefix{};
    const char header[] = "Polygon Movie maker 0002"; std::memcpy(prefix.data(), header, sizeof(header) - 1);
    for (std::size_t i = 30; i < prefix.size(); ++i) prefix[i] = static_cast<unsigned char>(i + 31);
    Check(_write(file.fd, prefix.data(), static_cast<unsigned>(prefix.size())) == prefix.size() &&
        pmm_extension::Append(file.fd, document), "encode production MMS records behind an unchanged standard PMM prefix");
    std::array<unsigned char, 64> actual{}; Check(_lseeki64(file.fd, 0, SEEK_SET) == 0 &&
        _read(file.fd, actual.data(), static_cast<unsigned>(actual.size())) == actual.size() && actual == prefix,
        "new comments never rewrite standard PMM prefix bytes");
    Check(_lseeki64(file.fd, 17, SEEK_SET) == 17, "set extension reader position");
    pmm_extension::Document decoded; std::uint64_t standard = 0;
    Check(pmm_extension::Read(file.fd, decoded, standard) == pmm_extension::ReadResult::Loaded &&
        standard == prefix.size() && _telli64(file.fd) == 17, "decode production MMS trailer and restore caller position");
    Check(decoded.records.size() == document.records.size(), "MMS record count survives encoding");
    for (std::size_t i = 0; i < document.records.size(); ++i)
        Check(decoded.records[i].key == document.records[i].key && decoded.records[i].value == document.records[i].value,
            "names v1 and independent comments payloads survive byte-exact trailer decoding");
    std::array<char, pmm_extension::kTrailerSize> trailer{};
    Check(_lseeki64(file.fd, -static_cast<__int64>(trailer.size()), SEEK_END) >= 0 &&
        _read(file.fd, trailer.data(), static_cast<unsigned>(trailer.size())) == trailer.size() &&
        !std::memcmp(trailer.data(), "MMSXPMM2", 8), "new comment records use only the MMS trailer marker");
    return decoded;
}
struct Before {
    struct Text { wchar_t** field; wchar_t* pointer; std::wstring value; };
    struct Group { mdl::ModelRecord* model; mdl::DisplayGroup* field; std::uint8_t flags; text_encoding::DisplayGroupTextState sidecar; };
    std::vector<Text> text;
    std::vector<Group> groups;
    std::size_t owned = owned_text::Count();
    explicit Before(MMDApp& app) {
        const auto add = [&](wchar_t*& field) { text.push_back({&field, field, field ? field : L""}); };
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& model = *mdl::Mdl(app.ModelSlot(slot));
            for (unsigned i = 0; i < 4; ++i) add(model.pmxTextBuffers[i]);
            for (unsigned i = 0; i < model.boneCount; ++i) { add(model.boneTable[i].jpText); add(model.boneTable[i].enText); }
            for (unsigned i = 0; i < model.morphCount; ++i) { add(model.morphs[i].jpText); add(model.morphs[i].enText); }
            auto* table = mdl::DisplayGroups(app.ModelSlot(slot));
            for (unsigned i = 0; i < mdl::DisplayGroupCount(model); ++i) {
                text_encoding::DisplayGroupTextState state;
                Check(text_encoding::CaptureDisplayGroupText(model, table[i], state), "capture source display sidecar");
                groups.push_back({&model, &table[i], table[i].flags, std::move(state)});
            }
        }
    }
    void Unchanged() const {
        Check(owned_text::Count() == owned, "failed project name/comment load frees every newly registered allocation");
        for (const auto& field : text)
            Check(*field.field == field.pointer && (!field.pointer || field.value == field.pointer),
                "failure leaves every model's original Unicode pointer and value untouched");
        for (const auto& group : groups)
            Check(group.field->flags == group.flags && text_encoding::MatchesDisplayGroupText(*group.model, *group.field, group.sidecar),
                "failure leaves every display flag and sidecar untouched");
    }
};
void Registered(wchar_t* value, const std::wstring& expected) {
    owned_text::Allocation allocation;
    Check(value && std::wstring(value) == expected && owned_text::Find(value, allocation) &&
        allocation.bytes == (expected.size() + 1) * sizeof(wchar_t), "loaded Unicode has an exact registered malloc allocation");
}
void RoundTrip() {
    model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
    auto expected = model_names_checks::Renamed(source.Read()); expected.comment += std::wstring(800, L'注');
    model_names_checks::Commit(source, expected);
    const std::wstring native = L"本地备注保留完整 Unicode 😀\n第二行" + std::wstring(600, L'文');
    auto& first = *mdl::Mdl(source.app->ModelSlot(0)); auto& second = *mdl::Mdl(source.app->ModelSlot(1));
    Check(extended_names::Assign(first.pmxTextBuffers[2], native) && extended_names::Assign(second.pmxTextBuffers[2], L""),
        "set independent local comments including an explicitly empty one");
    pmm_extension::Document document; pmm_extension::Set(document, "org.example.unknown", {1, 7, 3});
    extended_names::CapturePmm(*source.app, document);
    const auto* names = pmm_extension::Find(document, NamesKey(0)); const auto* comments = pmm_extension::Find(document, CommentsKey(0));
    Check(names && names->value.size() >= 4 && names->value[0] == 1 && comments && comments->value.size() >= 16 &&
        comments->value[0] == 1 && comments->value[4] == 3,
        "existing names v1 stays separate from versioned MMS comment records");
    auto decoded = Wire(document);
    model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
    extended_names::LoadPmm(*target.app, decoded);
    Check(model_names_checks::Same(target.Read(), expected) && target.Stable(), "real PMM extension roundtrip restores all edited names and English comment without changing pose");
    auto& loaded = *mdl::Mdl(target.app->ModelSlot(0)); Registered(loaded.pmxTextBuffers[2], native); Registered(loaded.pmxTextBuffers[3], expected.comment);
    Registered(mdl::Mdl(target.app->ModelSlot(1))->pmxTextBuffers[2], L"");
    Check(pmm_extension::Find(decoded, "org.example.unknown") && !target.history.UndoCount(), "project replacement preserves unknown records and creates no edit history");
    const auto* removed = source.app->ModelSlot(1); source.app->ModelSlot(1) = nullptr;
    extended_names::CapturePmm(*source.app, document); source.app->ModelSlot(1) = const_cast<unsigned char*>(removed);
    Check(!pmm_extension::Find(document, NamesKey(1)) && !pmm_extension::Find(document, CommentsKey(1)), "empty model slots erase both independent records");
}
void OldProjectsAndIndependentComments() {
    model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
    const auto expected = model_names_checks::Renamed(source.Read()); model_names_checks::Commit(source, expected);
    pmm_extension::Document document; extended_names::CapturePmm(*source.app, document);
    const auto commentRecord = *pmm_extension::Find(document, CommentsKey(0));
    for (unsigned slot = 0; slot < 2; ++slot) pmm_extension::Erase(document, CommentsKey(slot));
    model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
    auto& model = *mdl::Mdl(target.app->ModelSlot(0));
    Check(extended_names::Assign(model.pmxTextBuffers[2], L"source local comment") &&
        extended_names::Assign(model.pmxTextBuffers[3], L"source English comment"), "prepare source comments for an old project");
    auto* const local = model.pmxTextBuffers[2]; auto* const english = model.pmxTextBuffers[3];
    extended_names::LoadPmm(*target.app, Wire(document));
    Check(target.Read().model == expected.model && model.pmxTextBuffers[2] == local && model.pmxTextBuffers[3] == english &&
        std::wstring(local) == L"source local comment" && std::wstring(english) == L"source English comment",
        "old names-only v1 project does not replace either model-source comment");
    pmm_extension::Document commentsOnly; commentsOnly.records.push_back(commentRecord); auto* const name = model.pmxTextBuffers[1];
    extended_names::LoadPmm(*target.app, Wire(commentsOnly));
    Check(model.pmxTextBuffers[1] == name && std::wstring(model.pmxTextBuffers[3]) == expected.comment,
        "independent comment record loads without requiring or rewriting a names record");
    auto unknown = commentsOnly; pmm_extension::Set(unknown, CommentsKey(0), {2, 0, 0, 0, 0xff});
    auto* const unchanged = model.pmxTextBuffers[3]; extended_names::LoadPmm(*target.app, unknown);
    Check(model.pmxTextBuffers[3] == unchanged, "unknown comment versions retain original model comments");
}
void LoaderTruncatedComments() {
    std::array<char, 256> truncated{}; truncated.fill('a'); truncated[254] = static_cast<char>(0x82); truncated[255] = 0;
    const auto rawField = [](mdl::ModelRecord& model, unsigned language) { return language ? model.commentEn : model.comment; };
    const std::wstring valid = L"另一种语言的完整备注 😀";
    for (unsigned badLanguage = 0; badLanguage != 2; ++badLanguage) {
        model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
        auto& model = *mdl::Mdl(source.app->ModelSlot(0));
        owned_text::Free(model.pmxTextBuffers[2 + badLanguage]); model.pmxTextBuffers[2 + badLanguage] = nullptr;
        std::memcpy(rawField(model, badLanguage), truncated.data(), truncated.size());
        Check(extended_names::Assign(model.pmxTextBuffers[2 + (1 - badLanguage)], valid), "set valid Unicode beside a loader-truncated raw comment");
        pmm_extension::Document document; extended_names::CapturePmm(*source.app, document);
        const auto* record = pmm_extension::Find(document, CommentsKey(0));
        Check(record && record->value.size() >= 12 && record->value[4] == (1u << (1 - badLanguage)),
            "loader-style 254 ASCII plus isolated CP932 lead omits only that language without blocking PMM capture");
        for (bool sourceUnicode : {false, true}) {
            model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
            auto& loaded = *mdl::Mdl(target.app->ModelSlot(0));
            owned_text::Free(loaded.pmxTextBuffers[2 + badLanguage]); loaded.pmxTextBuffers[2 + badLanguage] = nullptr;
            std::memcpy(rawField(loaded, badLanguage), truncated.data(), truncated.size());
            if (sourceUnicode) Check(extended_names::Assign(loaded.pmxTextBuffers[2 + badLanguage], L"preserve current source comment"),
                "current model may now provide a canonical comment for the omitted language");
            auto* const unchanged = loaded.pmxTextBuffers[2 + badLanguage];
            extended_names::LoadPmm(*target.app, Wire(document));
            Check(loaded.pmxTextBuffers[2 + badLanguage] == unchanged &&
                !std::memcmp(rawField(loaded, badLanguage), truncated.data(), truncated.size()) &&
                (!unchanged || std::wstring(unchanged) == L"preserve current source comment"),
                "omitted language preserves its source pointer and raw truncated bytes without replacement characters");
            Registered(loaded.pmxTextBuffers[2 + (1 - badLanguage)], valid);
        }
        Check(extended_names::Assign(model.pmxTextBuffers[2 + badLanguage], L""), "explicit empty Unicode remains authoritative despite bad raw bytes");
        extended_names::CapturePmm(*source.app, document); record = pmm_extension::Find(document, CommentsKey(0));
        Check(record && record->value[4] == 3, "explicit empty string sets its language bit independently");
        model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
        extended_names::LoadPmm(*target.app, Wire(document));
        Registered(mdl::Mdl(target.app->ModelSlot(0))->pmxTextBuffers[2 + badLanguage], L"");
    }
    model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
    auto& model = *mdl::Mdl(source.app->ModelSlot(0));
    for (unsigned language = 0; language != 2; ++language) {
        owned_text::Free(model.pmxTextBuffers[2 + language]); model.pmxTextBuffers[2 + language] = nullptr;
        std::memcpy(rawField(model, language), truncated.data(), truncated.size());
    }
    pmm_extension::Document document; extended_names::CapturePmm(*source.app, document);
    const auto* record = pmm_extension::Find(document, CommentsKey(0));
    Check(record && record->value.size() == 8 && !record->value[4], "both unavailable comments produce a valid empty presence mask");
    model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
    auto& loaded = *mdl::Mdl(target.app->ModelSlot(0));
    Check(extended_names::Assign(loaded.pmxTextBuffers[2], L"keep local"), "prepare local source before an empty comment mask");
    auto* const local = loaded.pmxTextBuffers[2]; auto* const english = loaded.pmxTextBuffers[3];
    extended_names::LoadPmm(*target.app, Wire(document));
    Check(loaded.pmxTextBuffers[2] == local && loaded.pmxTextBuffers[3] == english,
        "empty presence mask preserves both original model-source comments");
}
void EmbeddedNulComments() {
    const std::wstring valid = L"other language is still restored completely 😀";
    const auto install = [](wchar_t*& field, const std::vector<unsigned char>& bytes) {
        auto* next = static_cast<wchar_t*>(std::malloc(bytes.size()));
        Check(next != nullptr, "allocate a complete early-NUL PMX comment");
        std::memcpy(next, bytes.data(), bytes.size()); owned_text::Remember(next, bytes.size());
        owned_text::Free(field); field = next;
    };
    for (unsigned language = 0; language != 2; ++language) for (unsigned variant = 0; variant != 3; ++variant) {
        const std::array<wchar_t, 8> payload = variant == 1 ?
            std::array<wchar_t, 8>{0,L'尾',L'部',0,L'Z',L'!',0,0} :
            variant == 2 ? std::array<wchar_t, 8>{L'A',0,0,0,0,0,0,0} :
            std::array<wchar_t, 8>{L'A',0,L'尾',L'部',0,L'Z',L'!',0};
        std::vector<unsigned char> bytes(sizeof(payload) + (variant == 2 ? 1 : 0));
        std::memcpy(bytes.data(), payload.data(), sizeof(payload)); if (variant == 2) bytes.back() = 0x7f;
        model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
        auto& model = *mdl::Mdl(source.app->ModelSlot(0)); install(model.pmxTextBuffers[2 + language], bytes);
        Check(extended_names::Assign(model.pmxTextBuffers[2 + (1 - language)], valid), "set the language beside a registered embedded-NUL comment");
        pmm_extension::Document document; extended_names::CapturePmm(*source.app, document);
        const auto* record = pmm_extension::Find(document, CommentsKey(0));
        Check(record && record->value[4] == (1u << (1 - language)),
            "nonzero UTF-16 or odd-byte allocation tails omit only that comment language, including an early empty prefix");
        model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
        auto& loaded = *mdl::Mdl(target.app->ModelSlot(0)); install(loaded.pmxTextBuffers[2 + language], bytes);
        auto* const original = loaded.pmxTextBuffers[2 + language]; owned_text::Allocation before;
        Check(owned_text::Find(original, before) && before.bytes == bytes.size(), "retain exact PMX comment source capacity");
        extended_names::LoadPmm(*target.app, Wire(document)); owned_text::Allocation after;
        Check(loaded.pmxTextBuffers[2 + language] == original && owned_text::Find(original, after) &&
            after.bytes == before.bytes && after.registryIdentity == before.registryIdentity &&
            !std::memcmp(original, bytes.data(), bytes.size()),
            "omitted embedded-NUL comments preserve their original pointer, registry allocation and every tail byte");
        Registered(loaded.pmxTextBuffers[2 + (1 - language)], valid);
        Check(!target.history.UndoCount() && target.Stable(), "embedded-NUL comment preservation changes no pose or history");
    }
    model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
    auto& model = *mdl::Mdl(source.app->ModelSlot(0)); std::vector<unsigned char> empty(9 * sizeof(wchar_t), 0);
    install(model.pmxTextBuffers[2], empty); install(model.pmxTextBuffers[3], empty);
    pmm_extension::Document document; extended_names::CapturePmm(*source.app, document);
    const auto* record = pmm_extension::Find(document, CommentsKey(0));
    Check(record && record->value.size() == 16 && record->value[4] == 3,
        "explicit empty Unicode with only zero allocation padding still records both languages");
    model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history);
    extended_names::LoadPmm(*target.app, Wire(document)); auto& loaded = *mdl::Mdl(target.app->ModelSlot(0));
    Registered(loaded.pmxTextBuffers[2], L""); Registered(loaded.pmxTextBuffers[3], L"");
}
void FailureAtomicity() {
    model_names_checks::Fixture source; NativePoseTestBinding sourceBinding(*source.app, source.history);
    auto expected = model_names_checks::Renamed(source.Read()); model_names_checks::Commit(source, expected);
    auto second = model_names_checks::Renamed(source.Read(1)); second.model = L"second independently edited model"; model_names_checks::Commit(source, second, 1);
    pmm_extension::Document document; extended_names::CapturePmm(*source.app, document); document = Wire(document);
    model_names_checks::Fixture target; NativePoseTestBinding targetBinding(*target.app, target.history); Before before(*target.app);
    for (unsigned malformed = 0; malformed != 6; ++malformed) {
        auto broken = document;
        const auto key = malformed == 2 ? NamesKey(1) : CommentsKey(malformed == 0 ? 1 : 0);
        auto value = pmm_extension::Find(broken, key)->value;
        if (malformed == 0 || malformed == 2) value.pop_back();
        if (malformed == 1) value = {1,0,0,0, 3,0,0,0, 2,0,0,0, 0xc0,0x80, 0,0,0,0};
        if (malformed == 3) value.push_back(0x7f);
        if (malformed == 4) value = {1,0,0,0, 4,0,0,0};
        if (malformed == 5) value = {1,0,0,0, 3,0,0,0, 0,0,0,0};
        pmm_extension::Set(broken, key, std::move(value)); bool rejected = false;
        try { extended_names::LoadPmm(*target.app, broken); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "truncated/invalid UTF-8/trailing bytes/unknown mask bits reject before any model is published"); before.Unchanged();
    }
    bool completed = false; unsigned failures = 0;
    for (long limit = 0; limit != 4096 && !completed; ++limit) {
        failAfter = limit;
        try { extended_names::LoadPmm(*target.app, document); completed = true; }
        catch (const std::bad_alloc&) { ++failures; }
        failAfter = -1;
        if (!completed) before.Unchanged();
    }
    Check(completed && failures > 30 && model_names_checks::Same(target.Read(), expected) &&
        model_names_checks::Same(target.Read(1), second), "allocation sweep spans all models, owned-text registration and sidecar preparation before one complete load");
    Check(!target.history.UndoCount() && target.Stable(), "project metadata replacement does not add history or evaluate poses");
}
struct PmdFields {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    std::array<mdl::DisplayGroup, 3> groups{};
    PmdFields() {
        std::memcpy(model->name, "12345678901234567890", 20); std::strcpy(model->nameEn, "old English");
        std::strcpy(model->comment, "local ASCII comment"); std::strcpy(model->commentEn, "English ASCII comment");
    }
    ~PmdFields() {
        text_encoding::ForgetDisplayGroupNames(*model); mdl::ForgetDisplayCapacity(*model);
        for (auto* pointer : model->pmxTextBuffers) owned_text::Free(pointer);
        owned_text::Free(bone.jpText); owned_text::Free(bone.enText);
    }
};
void PmdFidelity() {
    PmdFields fixture; auto& model = *fixture.model;
    Check(extended_names::CanSavePmdNames(model) && save_compatibility::CanSavePmd(model), "standard PMD retains a valid full-width 20-byte original model name");
    Check(extended_names::Assign(model.pmxTextBuffers[1], L"abcdefghijklmnopqrst"), "set a new exact twenty-byte English name");
    Check(extended_names::LegacyNamesFit(model, false) && !extended_names::CanSavePmdNames(model) && !save_compatibility::CanSavePmd(model),
        "representable twenty-byte Unicode with a preserved different raw mirror cannot silently export the old model name");
    std::memcpy(model.nameEn, "abcdefghijklmnopqrst", 20);
    Check(save_compatibility::CanSavePmd(model), "an exact matching full-width Unicode and raw name is valid standard PMD");
    Check(extended_names::Assign(model.pmxTextBuffers[3], L"comment outside CP932 😀") && !save_compatibility::CanSavePmd(model),
        "unrepresentable English comment rejects standard PMD instead of exporting an older comment");
    Check(extended_names::Assign(model.pmxTextBuffers[3], L"short ASCII comment"), "set a short canonical comment");
    Check(!save_compatibility::CanSavePmd(model), "even representable comment text must match the field the writer emits");
    std::memset(model.commentEn, 0, sizeof model.commentEn); std::strcpy(model.commentEn, "short ASCII comment");
    Check(save_compatibility::CanSavePmd(model), "matching short ASCII comment passes exact PMD fidelity");
    const std::wstring fullComment(256, L'c'); std::memset(model.commentEn, 'c', sizeof model.commentEn);
    Check(extended_names::Assign(model.pmxTextBuffers[3], fullComment) && save_compatibility::CanSavePmd(model),
        "full-width original 256-byte PMD comment remains representable");
    Check(extended_names::Assign(model.pmxTextBuffers[3], fullComment + L'c') && !save_compatibility::CanSavePmd(model),
        "Unicode comment beyond the standard field refuses instead of truncating");
    std::memset(model.commentEn, 0, sizeof model.commentEn); std::strcpy(model.commentEn, "short ASCII comment");
    Check(extended_names::Assign(model.pmxTextBuffers[3], L"short ASCII comment"), "restore matching English comment");
    Check(extended_names::Assign(model.pmxTextBuffers[2], L"local comment 😀") && !save_compatibility::CanSavePmd(model), "local comment is validated independently");
    Check(extended_names::Assign(model.pmxTextBuffers[2], L"local ASCII comment"), "restore matching local comment");
    std::memset(model.nameEn, 0, sizeof model.nameEn); Check(extended_names::Assign(model.pmxTextBuffers[1], L""), "clear English model name explicitly");
    Check(save_compatibility::CanSavePmd(model), "empty English is compared with its own empty field without local-name fallback");
    model.boneCount = 1; model.boneTable = &fixture.bone;
    std::memcpy(fixture.bone.name, "ABCDEFGHIJKLMNOPQRST", 20); std::memcpy(fixture.bone.nameEn, "abcdefghijklmnopqrst", 20);
    Check(save_compatibility::CanSavePmd(model), "standard PMD permits full twenty-byte original bone fields despite nineteen-byte UI mirror rules");
    Check(extended_names::Assign(fixture.bone.enText, L"different English") && !save_compatibility::CanSavePmd(model), "written bone canonical/raw mismatch refuses");
    owned_text::Free(fixture.bone.enText); fixture.bone.enText = nullptr;
    model.groupNames = fixture.groups.data(); mdl::SetDisplayCapacity(model, 3, 0);
    std::memset(fixture.groups[2].name, 'N', sizeof fixture.groups[2].name);
    const std::wstring nativeGroup(50, L'N'); text_encoding::SetDisplayGroupName(model, fixture.groups[2], nativeGroup.c_str(), L"");
    Check(save_compatibility::CanSavePmd(model), "actual fifty-byte PMD group and empty English sidecar use independent full-width fields");
}
} // namespace model_names_persistence_checks
void NativeModelNamesPersistenceChecks() {
    model_names_persistence_checks::RoundTrip();
    model_names_persistence_checks::OldProjectsAndIndependentComments();
    model_names_persistence_checks::LoaderTruncatedComments();
    model_names_persistence_checks::EmbeddedNulComments();
    model_names_persistence_checks::FailureAtomicity();
    model_names_persistence_checks::PmdFidelity();
}
