#include <mms/cpp/ui.hpp>
#include <mms/cpp/state_content.hpp>
#include <type_traits>

// Include after cpp_sdk_checks.inc: these failure-injecting tables forward to
// the real UiRegistry and retain the host's original handles and dependencies.
namespace cpp_input_sdk_checks {
namespace sdk = mms::sdk;
struct Host : cpp_sdk_checks::Host {
    std::int64_t value = 4;
    unsigned reads = 0, releases = 0, stages = 0;
    MmsChoiceV1 choices[2]{};
    Host() {
        for (auto& choice : choices) { choice.h = sdk::Header<MmsChoiceV1>(); choice.value.type = MMS_VALUE_INT; }
        choices[0].label = sdk::Text("four"); choices[0].value.value.signedValue = 4;
        choices[1].label = sdk::Text("eight"); choices[1].value.value.signedValue = 8;
    }
    static MmsResult MMS_CALL Read(void* context, MmsObjectRef, MmsValue* out, MmsHandle* lease) {
        auto& x = *static_cast<Host*>(context); ++x.reads;
        *out = {}; out->type = MMS_VALUE_INT; out->value.signedValue = x.value; *lease = 91; return MMS_OK;
    }
    static void MMS_CALL Release(void* context, MmsHandle lease) {
        if (lease == 91) ++static_cast<Host*>(context)->releases;
    }
    static MmsResult MMS_CALL Stage(void* context, MmsHandle, MmsObjectRef, const MmsValue*) {
        ++static_cast<Host*>(context)->stages; return MMS_OK;
    }
    sdk::PropertyInputSpec Input() {
        sdk::PropertyInputSpec spec{};
        spec.id = "example.cppsdk.amount"; spec.owner = client.id; spec.label = "Amount"; spec.unit = "units";
        spec.valueType = MMS_VALUE_INT; spec.context = this; spec.read = Read; spec.releaseValue = Release; spec.stage = Stage;
        spec.defaultValue.type = spec.minimum.type = spec.maximum.type = MMS_VALUE_INT;
        spec.defaultValue.value.signedValue = 4; spec.maximum.value.signedValue = 10;
        spec.choices = {choices, 2, sizeof(MmsChoiceV1), 1};
        spec.input = {"example.cppsdk.amount-input", {}, {}};
        return spec;
    }
    MmsPropertyDescV1 TypedProperty() {
        const auto spec = Input(); MmsPropertyDescV1 desc{}; desc.info.h = sdk::Header<MmsPropertyDescV1>();
        desc.info.id = sdk::Text(spec.id); desc.info.ownerPlugin = sdk::Text(spec.owner); desc.info.label = sdk::Text(spec.label);
        desc.info.unit = sdk::Text(spec.unit); desc.info.valueType = spec.valueType; desc.info.flags = spec.flags;
        desc.info.defaultValue = spec.defaultValue; desc.info.minimum = spec.minimum; desc.info.maximum = spec.maximum; desc.info.choices = spec.choices;
        desc.context = spec.context; desc.read = spec.read; desc.releaseValue = spec.releaseValue; desc.stage = spec.stage;
        return desc;
    }
};
static void Identity() {
    static_assert(!std::is_copy_constructible_v<sdk::PropertyInput>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::PropertyInput>);
    static_assert(!std::is_move_assignable_v<sdk::PropertyInput>);
    Host x; sdk::PropertyInput input(x.Services()); auto spec = x.Input();
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); hints.preferred[0] = 160; hints.preferred[1] = 24;
    spec.input.hints = &hints; spec.order = 17;
    CHECK(input.Register(spec) == MMS_OK && input.registrations().size() == 2 && input.cleanup_result() == MMS_OK);
    const auto property = input.property(), field = input.input();
    CHECK(property.kind == sdk::RegistrationKind::Property && field.kind == sdk::RegistrationKind::Element);
    CHECK(property.handle == x.registry.properties.at("example.cppsdk.amount")->handle && field.handle == x.registry.elements.at("example.cppsdk.amount-input").handle);
    CHECK(x.added.size() == 2 && x.added[0] == property.handle && x.added[1] == field.handle);
    const auto record = x.registry.properties.at("example.cppsdk.amount"); const auto& element = x.registry.elements.at("example.cppsdk.amount-input");
    CHECK(record->desc.context == &x && record->desc.read == Host::Read && record->desc.releaseValue == Host::Release && record->desc.stage == Host::Stage);
    CHECK(record->desc.info.valueType == MMS_VALUE_INT && !record->desc.info.components && record->desc.info.flags == MMS_PROPERTY_UNDOABLE);
    CHECK(String(record->desc.info.unit) == "units" && record->desc.info.defaultValue.value.signedValue == 4 && record->desc.info.maximum.value.signedValue == 10);
    CHECK(element.desc.kind == MMS_UI_INPUT && String(element.desc.propertyId) == spec.id && !element.desc.commandId.bytes && !element.desc.surfaceId.bytes);
    CHECK(String(element.desc.parentId) == "mms.toolbar.main.extensions" && String(element.desc.label) == "Amount");
    CHECK(element.defaultHints.preferred[0] == 160 && element.defaultHints.order == 17 && x.surfaceQueries == 0 && x.layoutQueries == 1 && x.queries == 2);
    x.choices[0].value.value.signedValue = 9;
    CHECK(static_cast<const MmsChoiceV1*>(record->desc.info.choices.data)[0].value.value.signedValue == 4);
    auto changed = record->desc; changed.info.label = sdk::Text("low-level replacement");
    CHECK(x.raw.updateProperty(x.raw.context, property.handle, &changed) == MMS_OK && input.property() == property);
    CHECK(x.registry.properties.at("example.cppsdk.amount")->handle == property.handle);
    CHECK(x.raw.unregisterItem(x.raw.context, property.handle) == MMS_E_BUSY);
    MmsValue value{}; MmsHandle lease = 0;
    CHECK(x.raw.readProperty(x.raw.context, sdk::Text(spec.id), {}, &value, &lease) == MMS_OK && value.value.signedValue == 4);
    CHECK(x.reads == 1 && x.releases == 1); x.raw.releaseValue(x.raw.context, lease);
    // Declaration never queries or turns on an editing service. The host has
    // no transaction bridge in this fixture, so direct staging stays closed.
    CHECK(x.raw.stageProperty(x.raw.context, 1, sdk::Text(spec.id), {}, &value) == MMS_E_NOT_READY && !x.stages);
    sdk::PropertyInput moved(std::move(input)); CHECK(!input.property() && !input.input() && moved.property() == property && moved.input() == field);
    CHECK(input.Close() == MMS_OK && moved.Close() == MMS_OK && !moved.property() && !moved.input());
    CHECK(x.removed.size() == 2 && x.removed[0] == field.handle && x.removed[1] == property.handle && x.registry.properties.empty());
}
static void ExistingProperty() {
    Host x; MmsHandle external = 0; auto desc = x.TypedProperty();
    CHECK(x.raw.registerProperty(x.raw.context, &desc, &external) == MMS_OK);
    sdk::RegistrationBundle bundle(x.Services()); sdk::RegistrationRef field;
    sdk::InputSpec spec{"example.cppsdk.existing-input", x.client.id, "Existing", {}, "example.cppsdk.amount"};
    CHECK(bundle.AddInput(spec, &field) == MMS_OK && bundle.size() == 1 && field.handle == x.registry.elements.at("example.cppsdk.existing-input").handle);
    CHECK(!bundle.owns({sdk::RegistrationKind::Property, external}) && bundle.Close() == MMS_OK);
    CHECK(x.registry.properties.at("example.cppsdk.amount")->handle == external && x.layoutQueries == 0);
    sdk::PropertyInput reference(x.Services()); CHECK(reference.Register(spec) == MMS_OK && !reference.property() && reference.input());
    CHECK(reference.Close() == MMS_OK && x.registry.properties.count("example.cppsdk.amount") == 1);
    CHECK(x.raw.unregisterItem(x.raw.context, external) == MMS_OK);
}
static void Failures() {
    for (unsigned failure : {1u, 2u}) {
        Host x; sdk::RegistrationBundle bundle(x.Services()); sdk::RegistrationRef earlier;
        CHECK(bundle.AddCommand(x.Command(), &earlier) == MMS_OK); x.failRegistration = x.registrations + failure;
        sdk::RegistrationRef property{sdk::RegistrationKind::Property, 123}, field{sdk::RegistrationKind::Element, 456};
        CHECK(bundle.AddPropertyInput(x.Input(), &property, &field) == MMS_E_FAILED && !property && !field);
        CHECK(bundle.cleanup_result() == MMS_OK && bundle.size() == 1 && bundle.owns(earlier) && x.registry.properties.empty());
        CHECK(x.registry.commands.at("example.cppsdk.command")->handle == earlier.handle);
        CHECK(std::find(x.attempted.begin(), x.attempted.end(), earlier.handle) == x.attempted.end());
        CHECK(bundle.Close() == MMS_OK);
    }
    for (MmsResult blocked : {MMS_E_BUSY, MMS_E_FAILED}) {
        Host x; sdk::PropertyInput control(x.Services()); sdk::RegistrationRef earlier;
        CHECK(control.registrations().AddCommand(x.Command(), &earlier) == MMS_OK);
        MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); auto spec = x.Input(); spec.input.hints = &hints;
        x.hintsResult = MMS_E_FAILED; x.removeResult = blocked;
        CHECK(control.Register(spec) == MMS_E_FAILED && control.cleanup_result() == blocked && !control.property() && !control.input());
        CHECK(control.registrations().size() == 3 && control.registrations().owns(earlier));
        const auto property = control.registrations().get(1), field = control.registrations().get(2);
        CHECK(property.handle == x.registry.properties.at("example.cppsdk.amount")->handle && field.handle == x.registry.elements.at("example.cppsdk.amount-input").handle);
        CHECK(std::find(x.attempted.begin(), x.attempted.end(), earlier.handle) == x.attempted.end());
        CHECK(control.Register(spec) == MMS_E_BUSY && control.registrations().size() == 3);
        x.removeResult = MMS_OK; CHECK(control.Close() == MMS_OK && !control.registrations().size() && x.registry.commands.empty() && x.registry.properties.empty());
    }
    // A failed INPUT-only composition keeps its older, owned property intact.
    Host x; sdk::RegistrationBundle bundle(x.Services()); sdk::RegistrationRef property;
    CHECK(bundle.AddProperty(x.TypedProperty(), &property) == MMS_OK);
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>();
    sdk::InputSpec spec{"example.cppsdk.input-only-fail", x.client.id, "Field", {}, "example.cppsdk.amount", &hints};
    x.hintsResult = MMS_E_FAILED; sdk::RegistrationRef field{sdk::RegistrationKind::Element, 123};
    CHECK(bundle.AddInput(spec, &field) == MMS_E_FAILED && !field && bundle.cleanup_result() == MMS_OK);
    CHECK(bundle.size() == 1 && bundle.owns(property) && x.registry.properties.at("example.cppsdk.amount")->handle == property.handle);
    CHECK(std::find(x.attempted.begin(), x.attempted.end(), property.handle) == x.attempted.end());
    CHECK(bundle.Close() == MMS_OK);
}
static void HostValidation() {
    Host unavailable; unavailable.layoutResult = MMS_E_NOT_FOUND;
    sdk::RegistrationBundle missing(unavailable.Services()); auto spec = unavailable.Input();
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); spec.input.hints = &hints;
    sdk::RegistrationRef property{sdk::RegistrationKind::Property, 1}, field{sdk::RegistrationKind::Element, 2};
    CHECK(missing.AddPropertyInput(spec, &property, &field) == MMS_E_NOT_FOUND && !property && !field && missing.size() == 0);
    CHECK(unavailable.registry.properties.empty() && missing.cleanup_result() == MMS_OK);
    // Layout remains optional for a normal adaptive field.
    spec.input.hints = nullptr; CHECK(missing.AddPropertyInput(spec, &property, &field) == MMS_OK && property && field);
    CHECK(missing.Close() == MMS_OK);

    Host x; sdk::RegistrationBundle bundle(x.Services()); const auto baseline = x.registry.elements.size();
    auto invalid = x.Input(); invalid.owner = "example.other";
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) != MMS_OK && !property && !field && bundle.size() == 0);
    invalid = x.Input(); invalid.input.parent = "mms.menu.plugins";
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) != MMS_OK && !property && !field && bundle.size() == 0 && x.registry.properties.empty());
    invalid = x.Input(); invalid.components = 2;
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) == MMS_E_ARGUMENT && !property && !field && bundle.size() == 0);
    invalid = x.Input(); invalid.stage = nullptr;
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) == MMS_E_NOT_READY && !property && !field && bundle.size() == 0);
    invalid = x.Input(); invalid.read = nullptr;
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) == MMS_E_ARGUMENT && !property && !field);
    invalid = x.Input(); invalid.input.id = {};
    CHECK(bundle.AddPropertyInput(invalid, &property, &field) == MMS_E_ARGUMENT && !property && !field);
    auto input = sdk::InputSpec{"example.cppsdk.missing-input", x.client.id, "Missing", {}, "example.cppsdk.absent"};
    CHECK(bundle.AddInput(input, &field) != MMS_OK && !field && bundle.size() == 0 && x.registry.elements.size() == baseline);
    // Owning an input does not grant access to another plugin's property.
    UiClient other{&x.registry, "example.other"}; auto otherApi = x.registry.Api(other); auto desc = x.TypedProperty();
    desc.info.id = sdk::Text("example.other.amount"); desc.info.ownerPlugin = sdk::Text("example.other"); MmsHandle external = 0;
    CHECK(otherApi.registerProperty(otherApi.context, &desc, &external) == MMS_OK);
    input.propertyId = "example.other.amount";
    CHECK(bundle.AddInput(input, &field) != MMS_OK && !field && bundle.size() == 0);
    CHECK(otherApi.unregisterItem(otherApi.context, external) == MMS_OK && bundle.Close() == MMS_OK);
}
static void ThreadBoundary() {
    Host x; sdk::RegistrationBundle bundle(x.Services()); sdk::PropertyInput control(x.Services());
    auto spec = x.Input(); sdk::RegistrationRef property{sdk::RegistrationKind::Property, 123}, field{sdk::RegistrationKind::Element, 456};
    MmsResult composite = MMS_OK, existing = MMS_OK, high = MMS_OK;
    std::thread worker([&] {
        composite = bundle.AddPropertyInput(spec, &property, &field);
        existing = bundle.AddInput({"example.cppsdk.worker", x.client.id, "Worker", {}, spec.id}, &field);
        high = control.Register(spec);
    }); worker.join();
    CHECK(composite == MMS_E_THREAD && existing == MMS_E_THREAD && high == MMS_E_THREAD && !property && !field);
    CHECK(x.registrations == 0 && x.layoutQueries == 0 && bundle.size() == 0 && control.registrations().size() == 0);
    CHECK(bundle.AddPropertyInput(spec, &property, &field) == MMS_OK); const auto count = x.registrations;
    MmsResult close = MMS_OK; std::thread other([&] { close = bundle.Close(); }); other.join();
    CHECK(close == MMS_E_THREAD && bundle.owns(property) && bundle.owns(field) && x.registrations == count);
    CHECK(bundle.Close() == MMS_OK && control.Close() == MMS_OK);
}
struct InputStateHost {
    MmsUiRefreshStateContentApiV1 api{};
    MmsHostV1 host{};
    uint32_t supported = MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION | MMS_UI_STATE_DISABLED_REASON |
        MMS_UI_STATE_ENABLED | MMS_UI_STATE_READ_ONLY | MMS_UI_STATE_VISIBLE;
    unsigned queries = 0, capabilities = 0, writes = 0;
    MmsResult queryResult = MMS_OK, capabilitiesResult = MMS_OK, writeResult = MMS_OK;
    bool validQuery = true;
    MmsHandle builder = 0;
    const MmsUiStateContentV1* pointer = nullptr;
    MmsUiInputStateContentV1 received{};
    std::string label, description, reason;
    InputStateHost() {
        api.h = sdk::Header<MmsUiRefreshStateContentApiV1>(); api.context = this;
        api.getCapabilities = [](void* p, MmsUiStateContentCapabilitiesV1* out) -> MmsResult {
            auto& x = *static_cast<InputStateHost*>(p); ++x.capabilities;
            CHECK(out && out->h.size == sizeof(*out) && out->h.major == 1 && !out->h.minor);
            *out = {sdk::Header<MmsUiStateContentCapabilitiesV1>(), x.supported, 128, 4096, 1024 * 1024, 8 * 1024 * 1024};
            return x.capabilitiesResult;
        };
        api.registerProvider = [](void*, const MmsUiStateContentDescV1*, MmsHandle*) -> MmsResult { return MMS_E_UNSUPPORTED; };
        api.updateProvider = [](void*, MmsHandle, const MmsUiStateContentDescV1*) -> MmsResult { return MMS_E_UNSUPPORTED; };
        api.unregisterProvider = [](void*, MmsHandle) -> MmsResult { return MMS_E_UNSUPPORTED; };
        api.writePrepared = [](void* p, MmsHandle builder, const MmsUiStateContentV1* content) -> MmsResult {
            auto& x = *static_cast<InputStateHost*>(p); ++x.writes; x.builder = builder; x.pointer = content; x.received = {};
            CHECK(content && content->h.size >= sizeof(*content)); x.received.content = *content;
            if (content->h.size >= sizeof(x.received)) std::memcpy(&x.received, content, sizeof(x.received));
            x.label = String(content->label); x.description = String(content->description); x.reason = String(content->disabledReason);
            return x.writeResult;
        };
        host.h = sdk::Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) -> MmsResult {
            auto& x = *static_cast<InputStateHost*>(p); ++x.queries;
            x.validQuery = x.validQuery && String(id) == "mms.ui.refresh.state" && major == 1 && minor == 0 && bytes == 56;
            if (x.queryResult) return x.queryResult;
            return Table(major, minor, bytes, out, x.api);
        };
    }
};
static void StateContentTail() {
    static_assert(sizeof(MmsUiStateContentV1) == 64 && sizeof(MmsUiRefreshStateContentApiV1) == 56);
    static_assert(sizeof(MmsUiInputStateContentV1) == 80 && offsetof(MmsUiInputStateContentV1, content) == 0);
    static_assert(std::is_standard_layout_v<MmsUiInputStateContentV1>);
    InputStateHost x; sdk::StateContentServices service;
    CHECK(sdk::StateContentServices::Query(x.host, service) == MMS_OK && service.ready() && x.queries == 1 && x.validQuery);
    CHECK(service.Write(41, "legacy label", "legacy description", "legacy reason") == MMS_OK &&
        x.builder == 41 && x.received.content.h.size == 64 && x.received.content.fields == 7 && !x.capabilities);
    CHECK(x.label == "legacy label" && x.description == "legacy description" && x.reason == "legacy reason");
    MmsUiInputStateContentV1 value{}; value.content.h = sdk::Header<MmsUiInputStateContentV1>();
    value.content.fields = MMS_UI_STATE_LABEL | MMS_UI_STATE_READ_ONLY; value.content.label = sdk::Text("same original builder"); value.readOnly = 1;
    CHECK(service.WriteInput(91, value) == MMS_OK && x.builder == 91 && x.pointer == &value.content &&
        x.received.content.h.size == 80 && x.received.content.fields == (MMS_UI_STATE_LABEL | MMS_UI_STATE_READ_ONLY) &&
        x.received.readOnly == 1 && !x.received.enabled && !x.received.visible && x.capabilities == 1);
    CHECK(service.WriteInput(92, "input", "help", "reason", false, true, false) == MMS_OK && x.builder == 92 &&
        x.received.content.fields == 63 && !x.received.enabled && x.received.readOnly == 1 && !x.received.visible);
    CHECK(service.WriteInput(93, "text only", "", "") == MMS_OK && x.received.content.fields == 7 &&
        x.received.content.h.size == 80 && !x.received.enabled && !x.received.readOnly && !x.received.visible);
    x.supported = 7; const auto writes = x.writes;
    CHECK(service.WriteInput(94, value) == MMS_E_UNSUPPORTED && x.writes == writes && x.builder == 93);
    CHECK(service.WriteInput(95, "legacy-capable text", "", "") == MMS_OK && x.builder == 95);
    x.supported = 63; CHECK(service.WriteInput(96, value) == MMS_OK && x.builder == 96);
    for (unsigned invalid = 0; invalid != 9; ++invalid) {
        auto malformed = value;
        if (invalid == 0) malformed.content.h.size = sizeof(MmsUiStateContentV1);
        if (invalid == 1) malformed.content.reserved = 1;
        if (invalid == 2) malformed.reserved = 1;
        if (invalid == 3) malformed.enabled = 1;
        if (invalid == 4) malformed.visible = 1;
        if (invalid == 5) malformed.readOnly = 2;
        if (invalid == 6) malformed.content.fields |= 64;
        if (invalid == 7) malformed.content.h.major = 2;
        if (invalid == 8) malformed.content.h.minor = 1;
        const auto priorWrites = x.writes, priorCaps = x.capabilities;
        CHECK(service.WriteInput(97, malformed) == (invalid >= 7 ? MMS_E_VERSION : MMS_E_ARGUMENT) &&
            x.writes == priorWrites && x.capabilities == priorCaps && x.builder == 96);
    }
    auto textOnly = value; textOnly.content.fields = MMS_UI_STATE_LABEL; textOnly.readOnly = 0; textOnly.content.h.size = 64;
    CHECK(service.WriteInput(98, textOnly) == MMS_E_ARGUMENT && x.builder == 96);
    x.capabilitiesResult = MMS_E_FAILED; MmsUiStateContentCapabilitiesV1 output{}; output.supportedFields = 777;
    CHECK(service.Capabilities(output) == MMS_E_FAILED && output.supportedFields == 777);
    CHECK(service.WriteInput(99, value) == MMS_E_FAILED && x.builder == 96);
    x.capabilitiesResult = MMS_OK; x.writeResult = MMS_E_BUSY;
    CHECK(service.WriteInput(100, value) == MMS_E_BUSY && x.builder == 100);
    x.queryResult = MMS_E_NOT_FOUND; const auto context = service.api().context;
    CHECK(sdk::StateContentServices::Query(x.host, service) == MMS_E_NOT_FOUND && service.ready() && service.api().context == context);
    const auto priorWrites = x.writes, priorCaps = x.capabilities; MmsResult typed = MMS_OK, convenience = MMS_OK;
    std::thread worker([&] { typed = service.WriteInput(101, value); convenience = service.WriteInput(102, "worker", "", "", true); }); worker.join();
    CHECK(typed == MMS_E_THREAD && convenience == MMS_E_THREAD && x.writes == priorWrites && x.capabilities == priorCaps);
    sdk::StateContentServices unavailable;
    CHECK(unavailable.WriteInput(103, value) == MMS_E_NOT_READY && x.writes == priorWrites);
}
} // namespace cpp_input_sdk_checks

static void CppInputSdkChecks() {
    cpp_input_sdk_checks::Identity();
    cpp_input_sdk_checks::ExistingProperty();
    cpp_input_sdk_checks::Failures();
    cpp_input_sdk_checks::HostValidation();
    cpp_input_sdk_checks::ThreadBoundary();
    cpp_input_sdk_checks::StateContentTail();
    std::cout << "C++ property INPUT SDK original handles/host validation/rollback/thread PASS\n";
}
