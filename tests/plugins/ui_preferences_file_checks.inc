#include "ui_preferences_file.hpp"
#include "mikumikustudio/system_settings.hpp"
static void UiPreferencesFileChecks(const fs::path& root) {
    struct Fixture {
        UiRegistry registry;
        UiClient client{&registry, "example.file"};
        MmsUiApiV1 api = registry.Api(client);
        Fixture() {
            MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>();
            e.id = S("example.file.group"); e.ownerPlugin = S("example.file");
            e.kind = MMS_UI_GROUP; e.parentId = S("mms.toolbar.main.extensions");
            e.capabilities = MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_CONTAINER;
            MmsHandle handle; CHECK(api.registerElement(api.context, &e, &handle) == MMS_OK);
        }
        void Visible(bool value) {
            MmsHandle tx; CHECK(api.beginLayout(api.context, registry.Revision(), &tx) == MMS_OK);
            MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_VISIBLE;
            op.elementId = S("example.file.group"); op.expectedGeneration = registry.elements.at("example.file.group").desc.generation;
            op.value.visible = value;
            CHECK(api.applyLayout(api.context, tx, &op) == MMS_OK); CHECK(api.commitLayout(api.context, tx) == MMS_OK);
        }
        void Size(float width) {
            MmsHandle tx; CHECK(api.beginLayout(api.context, registry.Revision(), &tx) == MMS_OK);
            MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_SIZE;
            op.elementId = S("example.file.group"); op.expectedGeneration = registry.elements.at("example.file.group").desc.generation;
            op.value.size = {width, 30};
            CHECK(api.applyLayout(api.context, tx, &op) == MMS_OK); CHECK(api.commitLayout(api.context, tx) == MMS_OK);
        }
    } first;
    const auto path = root / "layout-file" / "ui-layout.json";
    UiPreferencesFile a(path);
    CHECK(a.Save(first.registry) == MMS_E_NOT_READY);
    CHECK(a.Load(first.registry) == MMS_OK && a.Load(first.registry) == MMS_E_CONFLICT);
    first.Visible(false); CHECK(a.Save(first.registry) == MMS_OK);
    const auto initial = ReadFile(path); const auto initialTime = fs::last_write_time(path);
    CHECK(a.Save(first.registry) == MMS_OK && fs::last_write_time(path) == initialTime && ReadFile(path) == initial);
    Fixture second; UiPreferencesFile b(path); CHECK(b.Load(second.registry) == MMS_OK);
    CHECK(!second.registry.elements.at("example.file.group").visible);
    first.Size(180);
    const auto lockPath = fs::path(path.wstring() + L".lock");
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    const auto locked = a.Save(first.registry); CloseHandle(lock);
    CHECK(locked == MMS_E_BUSY && ReadFile(path) == initial);
    CHECK(a.Save(first.registry) == MMS_OK); const auto published = ReadFile(path);
    second.Size(240); CHECK(b.Save(second.registry) == MMS_E_CONFLICT && ReadFile(path) == published);
    Fixture third; UiPreferencesFile c(path); CHECK(c.Load(third.registry) == MMS_OK);
    CHECK(third.registry.elements.at("example.file.group").width == 180);
    third.Size(300);
    // Readers that forbid delete must prevent replacement without losing the old file.
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    const auto blocked = c.Save(third.registry); CloseHandle(reader);
    CHECK(blocked == MMS_E_FAILED && ReadFile(path) == published);
    for (const auto& entry : fs::directory_iterator(path.parent_path())) CHECK(entry.path().extension() != L".tmp");
    CHECK(c.Save(third.registry) == MMS_OK && ReadFile(path) != published);
    for (const auto& bytes : {std::string("{broken"), std::string("{\"schemaVersion\":2,\"elements\":[]}")}) {
        const auto malformed = root / "layout-invalid" / "ui-layout.json"; Write(malformed, bytes);
        Fixture local; UiPreferencesFile file(malformed);
        CHECK(file.Load(local.registry) != MMS_OK); local.Visible(false);
        CHECK(file.Save(local.registry) == MMS_E_NOT_READY && ReadFile(malformed) == bytes);
    }
    Fixture oversized; const auto large = root / "layout-large" / "ui-layout.json";
    Write(large, std::string(1024 * 1024 + 1, ' ')); UiPreferencesFile huge(large);
    CHECK(huge.Load(oversized.registry) != MMS_OK && huge.Save(oversized.registry) == MMS_E_NOT_READY);
    // Unified production mode migrates the old document once; later writers
    // compare only their own section, preserving settings from other modules.
    const auto unified = root / "unified-layout" / "settings.json";
    const auto legacy = root / "unified-layout" / "plugins" / "ui-layout.json";
    Write(legacy, initial);
    const auto oldTime = fs::last_write_time(legacy);
    Fixture unifiedFirst; UiPreferencesFile ua(legacy, true, unified);
    CHECK(ua.Load(unifiedFirst.registry) == MMS_OK);
    CHECK(!unifiedFirst.registry.elements.at("example.file.group").visible);
    mikumikustudio::system_settings::Store settings(unified);
    std::string section;
    CHECK(settings.ReadSection("pluginUi", section) == mikumikustudio::system_settings::Result::Ok);
    Fixture unifiedSecond; UiPreferencesFile ub(legacy, true, unified);
    CHECK(ub.Load(unifiedSecond.registry) == MMS_OK);
    CHECK(settings.WriteSection("history", "{\"recordAll\":1}") == mikumikustudio::system_settings::Result::Ok);
    unifiedFirst.Size(180); CHECK(ua.Save(unifiedFirst.registry) == MMS_OK);
    CHECK(settings.ReadSection("history", section) == mikumikustudio::system_settings::Result::Ok && Json::Parse(section).At("recordAll").UInt() == 1);
    unifiedSecond.Size(240); CHECK(ub.Save(unifiedSecond.registry) == MMS_E_CONFLICT);
    CHECK(ReadFile(legacy) == initial && fs::last_write_time(legacy) == oldTime);
    Write(legacy, "{invalid");
    Fixture unifiedThird; UiPreferencesFile uc(legacy, true, unified);
    CHECK(uc.Load(unifiedThird.registry) == MMS_OK);
    CHECK(unifiedThird.registry.elements.at("example.file.group").width == 180);
    const auto corrupt = root / "unified-corrupt" / "settings.json";
    Write(corrupt, "{invalid");
    Fixture unifiedCorrupt; UiPreferencesFile ud(legacy, true, corrupt);
    CHECK(ud.Load(unifiedCorrupt.registry) != MMS_OK && ud.Save(unifiedCorrupt.registry) == MMS_E_NOT_READY);
    CHECK(ReadFile(corrupt) == "{invalid");
}
