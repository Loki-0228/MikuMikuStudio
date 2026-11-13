#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace ui_property_stage_checks {
MmsValue Value(uint32_t type, double number = 2) {
    MmsValue v{}; v.type = type;
    switch (type) {
    case MMS_VALUE_BOOL: v.value.unsignedValue = 1; break;
    case MMS_VALUE_INT: v.value.signedValue = static_cast<int64_t>(number); break;
    case MMS_VALUE_UINT: v.value.unsignedValue = static_cast<uint64_t>(number); break;
    case MMS_VALUE_NUMBER: v.value.number = number; break;
    case MMS_VALUE_VECTOR: v.components = 2; v.value.vector[0] = number; v.value.vector[1] = number + 1; break;
    case MMS_VALUE_TEXT: v.value.text = S("original"); break;
    case MMS_VALUE_ID: v.value.id = {3, 5}; break;
    case MMS_VALUE_BYTES: { static const unsigned char bytes[]{1, 2, 3}; v.value.bytes = {bytes, sizeof(bytes)}; break; }
    }
    return v;
}
struct Fixture {
    unsigned changed = 0, finished = 0, stages = 0, reads = 0, releases = 0;
    UiRegistry registry{[this] { ++changed; }, [this](UiClient&, MmsResult) { ++finished; }};
    UiClient owner{&registry, "example.property"}, foreign{&registry, "example.foreign"};
    MmsUiApiV1 ui = registry.Api(owner), other = registry.Api(foreign);
    std::function<MmsResult(MmsHandle, const MmsValue&)> onStage;
    std::function<MmsResult(MmsValue*, MmsHandle*)> onRead;
    std::function<void()> onRelease;
    std::function<void()> onState;
    MmsValue current = Value(MMS_VALUE_NUMBER);
    MmsPropertyDescV1 desc{};
    MmsHandle handle = 0;
    Fixture(uint32_t type = MMS_VALUE_NUMBER, bool readonly = false) {
        current = Value(type);
        desc.info.h = Header<MmsPropertyDescV1>(); desc.info.id = S("example.property.value");
        desc.info.ownerPlugin = S("example.property"); desc.info.label = S("Property");
        desc.info.valueType = type; desc.info.components = current.components;
        desc.info.flags = readonly ? MMS_PROPERTY_READ_ONLY : MMS_PROPERTY_UNDOABLE;
        desc.context = this;
        desc.read = [](void* p, MmsObjectRef, MmsValue* value, MmsHandle* lease) -> MmsResult {
            auto& f = *static_cast<Fixture*>(p); ++f.reads;
            if (f.onRead) return f.onRead(value, lease);
            *value = f.current; *lease = 91; return MMS_OK;
        };
        desc.releaseValue = [](void* p, MmsHandle lease) {
            auto& f = *static_cast<Fixture*>(p); CHECK(lease == 91); ++f.releases;
            if (f.onRelease) f.onRelease();
        };
        desc.stage = [](void* p, MmsHandle tx, MmsObjectRef, const MmsValue* value) -> MmsResult {
            auto& f = *static_cast<Fixture*>(p); ++f.stages;
            return f.onStage ? f.onStage(tx, *value) : MMS_OK;
        };
    }
    void Register() { CHECK(ui.registerProperty(ui.context, &desc, &handle) == MMS_OK); }
    MmsResult Stage(const MmsValue& value, MmsHandle tx = 71) {
        return ui.stageProperty(ui.context, tx, desc.info.id, {}, &value);
    }
    void Bridge() {
        registry.SetPropertyStageBridge([this](UiClient& c, MmsHandle tx,
                const std::shared_ptr<PropertyRecord>& record, MmsObjectRef target, const MmsValue& value) {
            CHECK(&c == &owner && record->owner == &owner && tx == 71);
            return registry.WithPropertyStage(record, tx, [&](const PropertyStageFrame& frame) {
                CHECK(registry.IsPropertyStageCurrent(frame));
                return record->desc.stage(record->desc.context, tx, target, &value);
            });
        });
    }
};
void Schema() {
    for (uint32_t type = MMS_VALUE_BOOL; type <= MMS_VALUE_BYTES; ++type) {
        Fixture f(type); f.desc.info.defaultValue = Value(type); f.Register();
        CHECK(f.registry.properties.at("example.property.value")->desc.info.valueType == type);
    }
    const auto rejected = [](const std::function<void(Fixture&)>& change, MmsResult expected = MMS_E_ARGUMENT) {
        Fixture f; change(f); const auto revision = f.registry.Revision(); MmsHandle out = 0x12345;
        CHECK(f.ui.registerProperty(f.ui.context, &f.desc, &out) == expected);
        CHECK(out == 0x12345 && f.registry.Revision() == revision && f.registry.properties.empty() && !f.changed);
    };
    rejected([](Fixture& f) { f.desc.info.components = 1; });
    rejected([](Fixture& f) { f.desc.info.valueType = MMS_VALUE_VECTOR; f.desc.info.components = 0; });
    rejected([](Fixture& f) { f.desc.info.minimum = Value(MMS_VALUE_NUMBER, 4); f.desc.info.maximum = Value(MMS_VALUE_NUMBER, 3); });
    rejected([](Fixture& f) { f.desc.info.minimum = Value(MMS_VALUE_INT); });
    rejected([](Fixture& f) { f.desc.info.maximum = Value(MMS_VALUE_NUMBER, std::numeric_limits<double>::infinity()); });
    rejected([](Fixture& f) { f.desc.info.defaultValue = Value(MMS_VALUE_NUMBER, std::numeric_limits<double>::quiet_NaN()); });
    rejected([](Fixture& f) { f.desc.info.defaultValue = Value(MMS_VALUE_UINT); });
    rejected([](Fixture& f) { f.desc.info.defaultValue.components = 1; });
    rejected([](Fixture& f) { f.desc.info.minimum = Value(MMS_VALUE_NUMBER, 3); f.desc.info.defaultValue = Value(MMS_VALUE_NUMBER, 2); });
    rejected([](Fixture& f) { f.desc.info.maximum = Value(MMS_VALUE_NUMBER, 3); f.desc.info.defaultValue = Value(MMS_VALUE_NUMBER, 4); });
    rejected([](Fixture& f) { f.desc.info.valueType = MMS_VALUE_TEXT; f.desc.info.defaultValue = Value(MMS_VALUE_TEXT);
        static const char bad[]{char(0xc0), char(0xaf)}; f.desc.info.defaultValue.value.text = {bad, sizeof(bad)}; });
    rejected([](Fixture& f) { f.desc.info.valueType = MMS_VALUE_BYTES; f.desc.info.defaultValue = Value(MMS_VALUE_BYTES);
        f.desc.info.defaultValue.value.bytes = {nullptr, 1}; });
    rejected([](Fixture& f) { f.desc.info.valueType = MMS_VALUE_TEXT; f.desc.info.minimum = Value(MMS_VALUE_TEXT); });
    rejected([](Fixture& f) { f.desc.info.valueType = MMS_VALUE_BOOL; f.desc.info.maximum = Value(MMS_VALUE_BOOL); });
    rejected([](Fixture& f) { f.desc.info.flags = 0; }, MMS_E_NOT_READY);
    rejected([](Fixture& f) { f.desc.stage = nullptr; }, MMS_E_NOT_READY);
    rejected([](Fixture& f) { f.desc.info.flags |= MMS_PROPERTY_OBJECT; }, MMS_E_NOT_READY);
    rejected([](Fixture& f) { f.desc.info.flags |= MMS_PROPERTY_ANIMATABLE; }, MMS_E_NOT_READY);
    for (bool badDefault : {false, true}) {
        Fixture f; MmsChoiceV1 choices[2]{{Header<MmsChoiceV1>(), S("two"), Value(MMS_VALUE_NUMBER, 2)},
            {Header<MmsChoiceV1>(), S("four"), Value(MMS_VALUE_NUMBER, 4)}};
        f.desc.info.choices = {choices, 2, sizeof(MmsChoiceV1), 1};
        f.desc.info.minimum = Value(MMS_VALUE_NUMBER, 1); f.desc.info.maximum = Value(MMS_VALUE_NUMBER, badDefault ? 4 : 3);
        if (badDefault) f.desc.info.defaultValue = Value(MMS_VALUE_NUMBER, 3);
        MmsHandle out = 812; CHECK(f.ui.registerProperty(f.ui.context, &f.desc, &out) == MMS_E_ARGUMENT && out == 812);
    }
    Fixture vector(MMS_VALUE_VECTOR); vector.desc.info.minimum = Value(MMS_VALUE_VECTOR, 1);
    vector.desc.info.maximum = Value(MMS_VALUE_VECTOR, 3); vector.desc.info.minimum.value.vector[1] = 8;
    MmsHandle out = 100; CHECK(vector.ui.registerProperty(vector.ui.context, &vector.desc, &out) == MMS_E_ARGUMENT && out == 100);
    // Read-only schemas retain the established registration contract.
    Fixture legacy(MMS_VALUE_NUMBER, true); legacy.desc.info.components = 1; legacy.Register();
    CHECK(legacy.Stage(legacy.current, 0) == MMS_E_NOT_READY);
}
void ValuesAndOwnership() {
    Fixture f; f.desc.info.minimum = Value(MMS_VALUE_NUMBER, 1); f.desc.info.maximum = Value(MMS_VALUE_NUMBER, 4);
    MmsChoiceV1 choices[2]{{Header<MmsChoiceV1>(), S("two"), Value(MMS_VALUE_NUMBER, 2)},
        {Header<MmsChoiceV1>(), S("four"), Value(MMS_VALUE_NUMBER, 4)}};
    f.desc.info.choices = {choices, 2, sizeof(MmsChoiceV1), 1}; f.Register();
    CHECK(f.Stage(f.current) == MMS_E_NOT_READY); f.Bridge();
    CHECK(f.Stage(f.current) == MMS_OK && f.stages == 1 && !f.finished);
    const auto revision = f.registry.Revision(); const auto notifications = f.changed;
    for (const auto value : {Value(MMS_VALUE_NUMBER, 0), Value(MMS_VALUE_NUMBER, 3), Value(MMS_VALUE_NUMBER, 5),
            Value(MMS_VALUE_NUMBER, std::numeric_limits<double>::quiet_NaN()), Value(MMS_VALUE_INT, 2)})
        CHECK(f.Stage(value) == MMS_E_ARGUMENT);
    auto value = f.current; value.components = 1; CHECK(f.Stage(value) == MMS_E_ARGUMENT);
    CHECK(f.Stage(f.current, 0) == MMS_E_ARGUMENT);
    CHECK(f.ui.stageProperty(f.ui.context, 71, f.desc.info.id, {}, nullptr) == MMS_E_ARGUMENT);
    CHECK(f.ui.stageProperty(f.ui.context, 71, S("example.property.missing"), {}, &f.current) == MMS_E_NOT_FOUND);
    CHECK(f.other.stageProperty(f.other.context, 71, f.desc.info.id, {}, &f.current) == MMS_E_ARGUMENT);
    for (const auto target : {MmsObjectRef{{1, 0}, 0}, MmsObjectRef{{0, 1}, 0}, MmsObjectRef{{0, 0}, 1}})
        CHECK(f.ui.stageProperty(f.ui.context, 71, f.desc.info.id, target, &f.current) == MMS_E_ARGUMENT);
    f.owner.stopping = true; CHECK(f.Stage(f.current) == MMS_E_NOT_READY); f.owner.stopping = false;
    MmsResult wrongThread = MMS_OK; std::thread thread([&] { wrongThread = f.Stage(f.current); }); thread.join();
    CHECK(wrongThread == MMS_E_THREAD && f.stages == 1 && !f.finished);
    CHECK(f.registry.Revision() == revision && f.changed == notifications && !f.registry.InCallback());
    // The bridge receives owned text even if the source changes synchronously.
    Fixture text(MMS_VALUE_TEXT); text.Register(); std::string source = "original";
    auto textValue = text.current; textValue.value.text = Text(source);
    text.registry.SetPropertyStageBridge([&](UiClient&, MmsHandle tx, const std::shared_ptr<PropertyRecord>& record,
            MmsObjectRef target, const MmsValue& value) {
        CHECK(!text.registry.InCallback()); source.assign("replaced");
        CHECK(String(value.value.text) == "original" && value.value.text.data != source.data());
        return text.registry.WithPropertyStage(record, tx, [&](const PropertyStageFrame&) {
            return record->desc.stage(record->desc.context, tx, target, &value);
        });
    });
    CHECK(text.Stage(textValue) == MMS_OK && text.stages == 1);
    std::array<unsigned char, 3> bytes{3, 4, 5}; Fixture blob(MMS_VALUE_BYTES); blob.Register();
    auto blobValue = blob.current; blobValue.value.bytes = {bytes.data(), bytes.size()};
    blob.registry.SetPropertyStageBridge([&](UiClient&, MmsHandle, const std::shared_ptr<PropertyRecord>&,
            MmsObjectRef, const MmsValue& value) {
        bytes.fill(9); CHECK(value.value.bytes.data != bytes.data());
        CHECK(static_cast<const unsigned char*>(value.value.bytes.data)[0] == 3);
        return MMS_OK;
    });
    CHECK(blob.Stage(blobValue) == MMS_OK);
    MmsUiElementV1 input{}; input.h = Header<MmsUiElementV1>(); input.id = S("example.property.input");
    input.ownerPlugin = f.desc.info.ownerPlugin; input.parentId = S("mms.toolbar.main.extensions");
    input.kind = MMS_UI_INPUT; input.propertyId = f.desc.info.id; MmsHandle inputHandle = 400;
    CHECK(f.ui.registerElement(f.ui.context, &input, &inputHandle) == MMS_OK && inputHandle != 400);
    CHECK(f.registry.elements.at("example.property.input").handle == inputHandle);
}
void Frames() {
    Fixture f; f.Register(); const auto record = f.registry.properties.at("example.property.value");
    MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = S("example.property.run");
    command.info.ownerPlugin = f.desc.info.ownerPlugin; command.context = &f;
    command.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
        const auto& fixture = *static_cast<Fixture*>(p); if (fixture.onState) fixture.onState();
        *out = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK;
    };
    command.invoke = [](void* p, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult {
        auto& fixture = *static_cast<Fixture*>(p); return fixture.Stage(fixture.current);
    };
    MmsHandle commandHandle = 0; CHECK(f.ui.registerCommand(f.ui.context, &command, &commandHandle) == MMS_OK);
    const PropertyStageFrame* active = nullptr;
    f.registry.SetPropertyStageBridge([&](UiClient& c, MmsHandle tx, const std::shared_ptr<PropertyRecord>& property,
            MmsObjectRef target, const MmsValue& value) {
        CHECK(f.registry.InvokingClient() == &c);
        return f.registry.WithPropertyStage(property, tx, [&](const PropertyStageFrame& frame) {
            active = &frame; CHECK(f.registry.IsPropertyStageCurrent(frame));
            CHECK(f.registry.InvokingClient() == nullptr);
            CHECK(f.registry.WithPropertyStage(property, tx, [](const PropertyStageFrame&) { return MMS_OK; }) == MMS_E_BUSY);
            CHECK(f.ui.updateProperty(f.ui.context, f.handle, &f.desc) == MMS_E_BUSY);
            CHECK(f.ui.unregisterItem(f.ui.context, f.handle) == MMS_E_BUSY);
            CHECK(f.registry.WithReadOnlyCallback([&] {
                CHECK(!f.registry.IsPropertyStageCurrent(frame)); return MMS_OK;
            }) == MMS_OK);
            CHECK(f.registry.IsPropertyStageCurrent(frame));
            struct Raw { Fixture* f; const PropertyStageFrame* frame; } raw{&f, &frame};
            CHECK(f.registry.WithReadOnlyCallback([](void* p) -> MmsResult {
                const auto& data = *static_cast<Raw*>(p); CHECK(!data.f->registry.IsPropertyStageCurrent(*data.frame)); return MMS_OK;
            }, &raw) == MMS_OK);
            CHECK(f.registry.IsPropertyStageCurrent(frame));
            f.onRead = [&](MmsValue* result, MmsHandle* lease) {
                CHECK(!f.registry.IsPropertyStageCurrent(frame)); *result = f.current; *lease = 91; return MMS_OK;
            };
            f.onRelease = [&] { CHECK(!f.registry.IsPropertyStageCurrent(frame)); };
            MmsValue read{}; MmsHandle lease = 0;
            CHECK(f.ui.readProperty(f.ui.context, f.desc.info.id, {}, &read, &lease) == MMS_OK);
            f.ui.releaseValue(f.ui.context, lease); CHECK(f.registry.IsPropertyStageCurrent(frame));
            f.onState = [&] { CHECK(!f.registry.IsPropertyStageCurrent(frame)); };
            MmsCommandStateV1 state{}; state.h = Header<MmsCommandStateV1>(); MmsHandle stateLease = 0;
            CHECK(f.ui.getCommandState(f.ui.context, command.info.id, {}, &state, &stateLease) == MMS_OK);
            f.ui.releaseValue(f.ui.context, stateLease); f.onState = {};
            CHECK(f.registry.IsPropertyStageCurrent(frame));
            MmsResult wrong = MMS_OK; bool wrongCurrent = true, wrongRecord = true;
            std::thread thread([&] {
                wrongCurrent = f.registry.IsPropertyStageCurrent(frame);
                wrongRecord = f.registry.IsPropertyCurrent("example.property.value", property.get(), &f.owner);
                wrong = f.registry.WithPropertyStage(property, tx, [](const PropertyStageFrame&) { return MMS_OK; });
            }); thread.join(); CHECK(wrong == MMS_E_THREAD && !wrongCurrent && !wrongRecord);
            active = nullptr;
            return property->desc.stage(property->desc.context, tx, target, &value);
        });
    });
    CHECK(f.ui.invoke(f.ui.context, command.info.id, {}, {}) == MMS_OK);
    CHECK(f.stages == 1 && f.finished == 1 && !active && !f.registry.InCallback());
    const auto oldHandle = record->handle;
    CHECK(f.registry.IsPropertyCurrent("example.property.value", record.get(), &f.owner));
    CHECK(f.ui.updateProperty(f.ui.context, f.handle, &f.desc) == MMS_OK);
    CHECK(f.registry.properties.at("example.property.value")->handle == oldHandle);
    CHECK(f.registry.properties.at("example.property.value").get() != record.get());
    CHECK(!f.registry.IsPropertyCurrent("example.property.value", record.get(), &f.owner));
    CHECK(f.registry.WithPropertyStage(record, 71, [](const PropertyStageFrame&) { return MMS_OK; }) == MMS_E_STALE);
    auto current = f.registry.properties.at("example.property.value");
    CHECK(!f.registry.IsPropertyCurrent("example.property.value", current.get(), &f.foreign));
    CHECK(!f.registry.IsPropertyCurrent("wrong.id", current.get(), &f.owner));
    CHECK(f.registry.WithPropertyStage(current, 0, [](const PropertyStageFrame&) { return MMS_OK; }) == MMS_E_ARGUMENT);
    CHECK(f.registry.WithPropertyStage(current, 71, {}) == MMS_E_ARGUMENT);
    CHECK(f.registry.WithPropertyStage({}, 71, [](const PropertyStageFrame&) { return MMS_OK; }) == MMS_E_ARGUMENT);
    CHECK(f.registry.WithPropertyStage(current, 71, [&](const PropertyStageFrame& frame) -> MmsResult {
        CHECK(f.registry.IsPropertyStageCurrent(frame)); throw std::runtime_error("stage exception");
    }) == MMS_E_FAILED);
    CHECK(!f.registry.InCallback());
    CHECK(f.registry.WithPropertyStage(current, 71, [&](const PropertyStageFrame& frame) {
        CHECK(f.registry.IsPropertyStageCurrent(frame)); f.owner.stopping = true;
        CHECK(!f.registry.IsPropertyStageCurrent(frame)); f.owner.stopping = false; return MMS_OK;
    }) == MMS_OK);
    CHECK(f.registry.WithReadOnlyCallback(nullptr, nullptr) == MMS_E_ARGUMENT && !f.registry.InCallback());
}
void ReadLease() {
    for (int mode = 0; mode != 5; ++mode) {
        Fixture f(MMS_VALUE_TEXT); f.Register();
        f.onRead = [&](MmsValue* value, MmsHandle* lease) -> MmsResult {
            CHECK(f.registry.InCallback()); CHECK(f.ui.updateProperty(f.ui.context, f.handle, &f.desc) == MMS_E_BUSY);
            *lease = 91; *value = f.current;
            if (mode == 0) throw std::runtime_error("read after acquiring lease");
            if (mode == 1) return MMS_E_FAILED;
            if (mode == 2) value->type = MMS_VALUE_NUMBER;
            return MMS_OK;
        };
        f.onRelease = [&] {
            CHECK(f.registry.InCallback()); CHECK(f.ui.unregisterItem(f.ui.context, f.handle) == MMS_E_BUSY);
            if (mode == 3) throw std::runtime_error("release exception");
        };
        MmsValue output = Value(MMS_VALUE_INT, 812); MmsHandle lease = 0x45678;
        const auto result = f.ui.readProperty(f.ui.context, f.desc.info.id, {}, &output, &lease);
        CHECK(result == (mode == 4 ? MMS_OK : MMS_E_FAILED));
        CHECK(f.reads == 1 && f.releases == 1 && !f.registry.InCallback());
        if (mode != 4) CHECK(output.type == MMS_VALUE_INT && output.value.signedValue == 812 && lease == 0x45678);
        else {
            CHECK(output.type == MMS_VALUE_TEXT && String(output.value.text) == "original" && lease != 0x45678);
            f.ui.releaseValue(f.ui.context, lease); CHECK(f.releases == 1);
        }
        CHECK(f.owner.unsafeToUnload == (mode == 3));
        CHECK(f.registry.HasLeases(f.owner) == (mode == 3));
    }
}
}
static void UiPropertyStageChecks() {
    ui_property_stage_checks::Schema();
    ui_property_stage_checks::ValuesAndOwnership();
    ui_property_stage_checks::Frames();
    ui_property_stage_checks::ReadLease();
}
