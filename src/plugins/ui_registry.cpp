#include "ui_registry.hpp"
#include <cmath>
#include <set>

namespace mikudancestudio::plugins {
namespace {
template<class T, class F> MmsSpan CopySpan(Arena& arena, MmsSpan in, F copy) {
    if (in.count == 0) return {nullptr, 0, sizeof(T), 1};
    if (!in.data || in.count > 4096 || in.stride < sizeof(T) || in.stride > 65536 || in.recordVersion != 1 || in.count > SIZE_MAX / in.stride)
        throw std::invalid_argument("invalid span");
    auto bytes = std::make_unique<unsigned char[]>(size_t(in.count) * sizeof(T)); auto* out = reinterpret_cast<T*>(bytes.get());
    for (size_t i = 0; i < in.count; ++i) {
        T item; std::memcpy(&item, static_cast<const unsigned char*>(in.data) + i * in.stride, sizeof(item));
        if (item.h.size < sizeof(item) || item.h.size > in.stride || item.h.major != 1) throw std::invalid_argument("invalid span record");
        out[i] = copy(item); out[i].h = Header<T>();
    }
    arena.blocks.push_back(std::move(bytes)); return {out, in.count, sizeof(T), 1};
}
bool Null(MmsObjectRef target) { return !target.id.lo && !target.id.hi && !target.generation; }
struct Callback { unsigned& depth; explicit Callback(unsigned& n): depth(n) { ++depth; } ~Callback() { --depth; } };
template<class T> auto ByHandle(std::map<std::string, std::shared_ptr<T>>& records, MmsHandle h) {
    return std::find_if(records.begin(), records.end(), [h](const auto& x) { return x.second->handle == h; });
}
bool IsMenu(uint32_t kind) { return kind == MMS_UI_MENU || kind == MMS_UI_MENU_ITEM || kind == MMS_UI_SEPARATOR; }
MmsUiLayoutHintsV1 InitialHints() { MmsUiLayoutHintsV1 h{}; h.h = Header<MmsUiLayoutHintsV1>(); return h; }
MmsResult ValidateHints(const MmsUiLayoutHintsV1* h) {
    if (!Valid(h) || h->flow > 4 || h->columns > 4096 || h->align > 3 || h->overflow > 3 || h->flags & ~7u) return MMS_E_ARGUMENT;
    // Command overflow menus need an input-preserving adapter first.
    if (h->overflow == 3) return MMS_E_UNSUPPORTED;
    for (const auto* values : {h->minimum, h->preferred, h->maximum, h->grow, h->shrink, h->gap})
        for (int axis = 0; axis < 2; ++axis) if (!std::isfinite(values[axis]) || values[axis] < 0 || values[axis] > 32768) return MMS_E_ARGUMENT;
    for (float value : h->padding) if (!std::isfinite(value) || value < 0 || value > 32768) return MMS_E_ARGUMENT;
    for (int axis = 0; axis < 2; ++axis) if (h->maximum[axis] && h->maximum[axis] < h->minimum[axis]) return MMS_E_ARGUMENT;
    return MMS_OK;
}
bool EqualValue(const MmsValue& a, const MmsValue& b) {
    if (a.type != b.type || a.components != b.components) return false;
    switch (a.type) {
    case MMS_VALUE_EMPTY: return true;
    case MMS_VALUE_BOOL: case MMS_VALUE_UINT: return a.value.unsignedValue == b.value.unsignedValue;
    case MMS_VALUE_INT: return a.value.signedValue == b.value.signedValue;
    case MMS_VALUE_NUMBER: return a.value.number == b.value.number;
    case MMS_VALUE_VECTOR: for (unsigned n = 0; n < a.components; ++n) if (a.value.vector[n] != b.value.vector[n]) return false; return true;
    case MMS_VALUE_TEXT: return String(a.value.text) == String(b.value.text);
    case MMS_VALUE_ID: return a.value.id.lo == b.value.id.lo && a.value.id.hi == b.value.id.hi;
    case MMS_VALUE_BYTES: return a.value.bytes.bytes == b.value.bytes.bytes && (!a.value.bytes.bytes || std::memcmp(a.value.bytes.data, b.value.bytes.data, size_t(a.value.bytes.bytes)) == 0);
    default: return false;
    }
}
bool Range(const MmsValue& value, const MmsValue& bound, bool lower) {
    if (bound.type == MMS_VALUE_EMPTY) return true;
    if (value.type != bound.type || value.components != bound.components) return false;
    switch (value.type) {
    case MMS_VALUE_INT: return lower ? value.value.signedValue >= bound.value.signedValue : value.value.signedValue <= bound.value.signedValue;
    case MMS_VALUE_UINT: case MMS_VALUE_BOOL: return lower ? value.value.unsignedValue >= bound.value.unsignedValue : value.value.unsignedValue <= bound.value.unsignedValue;
    case MMS_VALUE_NUMBER: return lower ? value.value.number >= bound.value.number : value.value.number <= bound.value.number;
    case MMS_VALUE_VECTOR:
        for (unsigned i = 0; i < value.components; ++i) if (lower ? value.value.vector[i] < bound.value.vector[i] : value.value.vector[i] > bound.value.vector[i]) return false;
        return true;
    default: return false;
    }
}
bool Fits(const MmsValue& value, uint32_t type, uint32_t components, const MmsValue& minimum, const MmsValue& maximum, MmsSpan choices) {
    if (value.type != type || value.components != components || !Range(value, minimum, true) || !Range(value, maximum, false)) return false;
    if (!choices.count) return true;
    for (size_t i = 0; i < choices.count; ++i) {
        const auto& choice = *reinterpret_cast<const MmsChoiceV1*>(static_cast<const unsigned char*>(choices.data) + choices.stride * i);
        if (EqualValue(value, choice.value)) return true;
    }
    return false;
}
bool SameArguments(MmsSpan a, MmsSpan b) {
    if (a.count != b.count) return false;
    for (size_t i = 0; i < a.count; ++i) {
        const auto& x = *reinterpret_cast<const MmsArgumentV1*>(static_cast<const unsigned char*>(a.data) + i * a.stride);
        const auto& y = *reinterpret_cast<const MmsArgumentV1*>(static_cast<const unsigned char*>(b.data) + i * b.stride);
        if (String(x.name) != String(y.name) || x.valueType != y.valueType || x.components != y.components) return false;
    }
    return true;
}
}
MmsString Arena::Copy(MmsString s) { strings.push_back(String(s)); return Text(strings.back()); }
MmsValue Arena::Copy(const MmsValue& v) {
    MmsValue r = v;
    if (v.type > MMS_VALUE_BYTES || (v.type == MMS_VALUE_VECTOR && (!v.components || v.components > 4))) throw std::invalid_argument("invalid value");
    if (v.type == MMS_VALUE_NUMBER && !std::isfinite(v.value.number)) throw std::invalid_argument("non-finite number");
    if (v.type == MMS_VALUE_VECTOR) for (unsigned n = 0; n < v.components; ++n) if (!std::isfinite(v.value.vector[n])) throw std::invalid_argument("non-finite vector");
    if (v.type == MMS_VALUE_BOOL && v.value.unsignedValue > 1) throw std::invalid_argument("invalid boolean");
    if (v.type == MMS_VALUE_TEXT) r.value.text = Copy(v.value.text);
    if (v.type == MMS_VALUE_BYTES) {
        if (v.value.bytes.bytes > 16 * 1024 * 1024 || (v.value.bytes.bytes && !v.value.bytes.data)) throw std::invalid_argument("invalid bytes");
        auto bytes = std::make_unique<unsigned char[]>(size_t(v.value.bytes.bytes));
        if (v.value.bytes.bytes) std::memcpy(bytes.get(), v.value.bytes.data, size_t(v.value.bytes.bytes));
        r.value.bytes.data = bytes.get(); blocks.push_back(std::move(bytes));
    }
    return r;
}
MmsSpan Arena::Choices(MmsSpan s) {
    return CopySpan<MmsChoiceV1>(*this, s, [&](auto c) { c.label = Copy(c.label); c.value = Copy(c.value); return c; });
}
MmsSpan Arena::Arguments(MmsSpan s) {
    std::set<std::string> names;
    return CopySpan<MmsArgumentV1>(*this, s, [&](auto a) {
        auto name = String(a.name);
        if (!ValidId(name) || !names.insert(name).second || a.valueType < MMS_VALUE_BOOL || a.valueType > MMS_VALUE_BYTES || a.reserved || a.flags > MMS_ARGUMENT_REQUIRED || a.components > 4)
            throw std::invalid_argument("invalid argument schema");
        a.name = Copy(a.name); a.defaultValue = Copy(a.defaultValue); a.minimum = Copy(a.minimum); a.maximum = Copy(a.maximum); a.choices = Choices(a.choices);
        for (size_t i = 0; i < a.choices.count; ++i) {
            const auto& choice = static_cast<const MmsChoiceV1*>(a.choices.data)[i];
            if (!Fits(choice.value, a.valueType, a.components, a.minimum, a.maximum, {})) throw std::invalid_argument("invalid choice");
        }
        if (a.defaultValue.type != MMS_VALUE_EMPTY && !Fits(a.defaultValue, a.valueType, a.components, a.minimum, a.maximum, a.choices)) throw std::invalid_argument("invalid default value");
        if (!(a.flags & MMS_ARGUMENT_REQUIRED) && a.defaultValue.type == MMS_VALUE_EMPTY) throw std::invalid_argument("optional argument needs default");
        return a;
    });
}
struct UiRegistry::Lease {
    std::string owner;
    unsigned kind = 0; // 1 snapshot, 2 value, 3 anchors
    Arena arena;
    std::vector<std::shared_ptr<CommandRecord>> commandOwners;
    std::vector<std::shared_ptr<PropertyRecord>> propertyOwners;
    std::vector<ElementRecord> elementOwners;
    std::vector<MmsCommandInfoV1> commands;
    std::vector<MmsPropertyInfoV1> properties;
    std::vector<MmsUiElementV1> elements;
    std::vector<MmsUiAnchorV1> anchors;
    std::vector<MmsUiLayoutItemV1> geometry;
};
struct UiRegistry::Layout {
    std::string owner; uint64_t revision;
    Arena arena; std::vector<MmsLayoutOpV1> operations;
    struct Hint { std::string id; uint64_t generation; MmsUiLayoutHintsV1 value; };
    std::vector<Hint> hints;
};
struct UiRegistry::View {
    std::shared_ptr<SurfaceRecord> surface;
    void* context = nullptr;
    HWND child = nullptr, parent = nullptr;
    MmsUiRectV1 bounds{};
    uint32_t dpi = 0;
    bool visible = false;
};
UiRegistry::UiRegistry(std::function<void()> change): changed_(std::move(change)) {
    for (const auto& [id, label, kind] : {std::tuple<const char*, const char*, uint32_t>{"mms.menu.plugins", "插件", MMS_UI_MENU}, {"mms.toolbar.main.extensions", "插件工具栏", MMS_UI_TOOLBAR}, {"mms.workspace.tabs.extensions", "插件工作区", MMS_UI_GROUP}}) {
        ElementRecord e; e.handle = next_++; e.arena = std::make_shared<Arena>(); e.desc.h = Header<MmsUiElementV1>();
        e.desc.id = e.arena->Copy(Text(id)); e.desc.ownerPlugin = e.arena->Copy(Text("mms")); e.desc.label = e.arena->Copy(Text(label));
        e.desc.kind = kind; e.desc.capabilities = MMS_UI_CONTAINER | (kind == MMS_UI_MENU ? 0 : MMS_UI_LAYOUT); e.desc.generation = generation_++;
        e.defaultHints = e.effectiveHints = InitialHints(); elements.emplace(id, std::move(e));
    }
}
UiRegistry::~UiRegistry() = default;
MmsResult UiRegistry::Check(UiClient& c, bool mutate) const {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (mutate && (callbackDepth_ || c.stopping)) return c.stopping ? MMS_E_NOT_READY : MMS_E_BUSY;
    return MMS_OK;
}
bool UiRegistry::Own(UiClient& c, MmsString id, MmsString owner) const {
    const auto s = String(id); return ValidId(s) && String(owner) == c.id && s.rfind(c.id + ".", 0) == 0;
}
bool UiRegistry::Exists(const std::string& id) const { return commands.count(id) || elements.count(id) || properties.count(id) || surfaces.count(id); }
void UiRegistry::Changed() { ++revision_; if (changed_) try { changed_(); } catch (...) {} }
MmsResult UiRegistry::Command(UiClient& c, const MmsCommandDescV1* input, MmsHandle* out, MmsHandle replace) {
    auto check = Check(c, true); if (check) return check;
    if (!input || input->info.h.size < sizeof(*input) || input->info.h.major != 1 || input->info.h.minor != 0 || !out || !Own(c, input->info.id, input->info.ownerPlugin) ||
        !input->getState || !input->invoke || input->info.reserved || (input->info.flags & ~7u)) return MMS_E_ARGUMENT;
    if (input->info.flags & (MMS_COMMAND_EDIT | MMS_COMMAND_OBJECT)) return MMS_E_NOT_READY;
    if ((input->info.flags & MMS_COMMAND_INVOKE_ONLY) && input->info.arguments.count) return MMS_E_ARGUMENT;
    const auto id = String(input->info.id); auto found = commands.find(id);
    if (replace ? found == commands.end() || found->second->handle != replace : Exists(id)) return replace ? MMS_E_NOT_FOUND : MMS_E_CONFLICT;
    auto r = std::make_shared<CommandRecord>(); r->owner = &c; r->handle = replace ? replace : next_++; r->arena = std::make_shared<Arena>(); r->desc = *input;
    auto& d = r->desc.info; d.h = Header<MmsCommandDescV1>();
    d.id = r->arena->Copy(d.id); d.ownerPlugin = r->arena->Copy(d.ownerPlugin); d.label = r->arena->Copy(d.label); d.description = r->arena->Copy(d.description); d.arguments = r->arena->Arguments(d.arguments);
    if (replace && (found->second->desc.context != input->context || !SameArguments(found->second->desc.info.arguments, d.arguments))) return MMS_E_ARGUMENT;
    commands[id] = r; *out = r->handle; Changed(); return MMS_OK;
}
bool UiRegistry::Allowed(const MmsUiElementV1& e, const std::map<std::string, ElementRecord>& tree) const {
    auto p = tree.find(String(e.parentId)); if (p == tree.end() || !(p->second.desc.capabilities & MMS_UI_CONTAINER)) return false;
    std::set<std::string> visited{String(e.id)};
    while (p != tree.end()) {
        auto id = p->first; if (!visited.insert(id).second) return false;
        if (id == "mms.menu.plugins") return IsMenu(e.kind);
        if (id == "mms.toolbar.main.extensions") return e.kind == MMS_UI_BUTTON || e.kind == MMS_UI_SEPARATOR || e.kind == MMS_UI_GROUP;
        if (id == "mms.workspace.tabs.extensions") return e.kind == MMS_UI_PANEL;
        if (p->second.desc.kind != MMS_UI_MENU && p->second.desc.kind != MMS_UI_GROUP) return false;
        p = tree.find(String(p->second.desc.parentId));
    }
    return false;
}
MmsResult UiRegistry::Element(UiClient& c, const MmsUiElementV1* input, MmsHandle* out, MmsHandle replace) {
    auto check = Check(c, true); if (check) return check;
    if (!Valid(input) || !out || !Own(c, input->id, input->ownerPlugin) || input->kind < MMS_UI_MENU || input->kind > MMS_UI_GROUP || (input->capabilities & ~127u)) return MMS_E_ARGUMENT;
    if (input->kind == MMS_UI_INPUT || input->propertyId.bytes) return MMS_E_NOT_READY;
    if (input->kind == MMS_UI_PANEL) {
        auto surface = surfaces.find(String(input->surfaceId));
        if (surface == surfaces.end()) return MMS_E_NOT_FOUND;
        if (surface->second->owner != &c) return MMS_E_ARGUMENT;
        if (!(surface->second->desc.flags & MMS_SURFACE_MULTIPLE_VIEWS))
            for (const auto& [id, e] : elements) if (e.handle != replace && String(e.desc.surfaceId) == surface->first) return MMS_E_BUSY;
    } else if (input->surfaceId.bytes) return MMS_E_ARGUMENT;
    if ((input->kind == MMS_UI_MENU || input->kind == MMS_UI_GROUP) != !!(input->capabilities & MMS_UI_CONTAINER)) return MMS_E_ARGUMENT;
    if (input->capabilities & MMS_UI_DOCK) return MMS_E_UNSUPPORTED;
    if ((input->capabilities & MMS_UI_LAYOUT) && (input->kind == MMS_UI_MENU || input->kind == MMS_UI_MENU_ITEM)) return MMS_E_UNSUPPORTED;
    const auto id = String(input->id); auto old = elements.find(id);
    if (replace ? old == elements.end() || old->second.handle != replace : Exists(id)) return replace ? MMS_E_NOT_FOUND : MMS_E_CONFLICT;
    if ((input->kind == MMS_UI_BUTTON || input->kind == MMS_UI_MENU_ITEM) && !commands.count(String(input->commandId))) return MMS_E_NOT_FOUND;
    if (!Allowed(*input, elements)) return MMS_E_ARGUMENT;
    ElementRecord r; if (replace) r = old->second;
    r.handle = replace ? replace : next_++; r.arena = std::make_shared<Arena>(); r.desc = *input; r.desc.h = Header<MmsUiElementV1>();
    auto& d = r.desc; d.id = r.arena->Copy(d.id); d.ownerPlugin = r.arena->Copy(d.ownerPlugin); d.parentId = r.arena->Copy(d.parentId); d.label = r.arena->Copy(d.label);
    d.commandId = r.arena->Copy(d.commandId); d.propertyId = r.arena->Copy(d.propertyId); d.surfaceId = r.arena->Copy(d.surfaceId); d.generation = generation_++;
    r.defaultParent = String(d.parentId); if (!replace) { r.order = r.handle; r.defaultHints = InitialHints(); r.defaultHints.order = int32_t(r.order); r.effectiveHints = r.defaultHints; }
    if (replace) DestroyView(id);
    elements[id] = std::move(r); *out = elements[id].handle; Changed(); return MMS_OK;
}
MmsResult UiRegistry::Property(UiClient& c, const MmsPropertyDescV1* input, MmsHandle* out, MmsHandle replace) {
    auto check = Check(c, true); if (check) return check;
    if (!input || input->info.h.size < sizeof(*input) || input->info.h.major != 1 || input->info.h.minor != 0 || !out || !Own(c, input->info.id, input->info.ownerPlugin) ||
        !input->read || !input->releaseValue || input->info.reserved || input->info.flags & ~15u || input->info.valueType < 1 || input->info.valueType > 8 || input->info.components > 4) return MMS_E_ARGUMENT;
    if (!(input->info.flags & MMS_PROPERTY_READ_ONLY) || input->info.flags & (MMS_PROPERTY_OBJECT | MMS_PROPERTY_ANIMATABLE)) return MMS_E_NOT_READY;
    const auto id = String(input->info.id); auto found = properties.find(id);
    if (replace ? found == properties.end() || found->second->handle != replace : Exists(id)) return replace ? MMS_E_NOT_FOUND : MMS_E_CONFLICT;
    auto r = std::make_shared<PropertyRecord>(); r->owner = &c; r->handle = replace ? replace : next_++; r->arena = std::make_shared<Arena>(); r->desc = *input;
    auto& d = r->desc.info; d.h = Header<MmsPropertyDescV1>(); d.id = r->arena->Copy(d.id); d.ownerPlugin = r->arena->Copy(d.ownerPlugin); d.label = r->arena->Copy(d.label); d.unit = r->arena->Copy(d.unit);
    d.defaultValue = r->arena->Copy(d.defaultValue); d.minimum = r->arena->Copy(d.minimum); d.maximum = r->arena->Copy(d.maximum); d.choices = r->arena->Choices(d.choices);
    if (replace && (found->second->desc.context != input->context || found->second->desc.info.valueType != d.valueType || found->second->desc.info.components != d.components)) return MMS_E_ARGUMENT;
    properties[id] = r; *out = r->handle; Changed(); return MMS_OK;
}
MmsResult UiRegistry::Remove(UiClient& c, MmsHandle handle) {
    auto check = Check(c); if (check) return check; if (callbackDepth_) return MMS_E_BUSY;
    for (auto it = elements.begin(); it != elements.end(); ++it) if (it->second.handle == handle) {
        if (String(it->second.desc.ownerPlugin) != c.id) return MMS_E_ARGUMENT;
        for (const auto& [id, e] : elements) if (String(e.desc.parentId) == it->first) return MMS_E_BUSY;
        DestroyView(it->first);
        elements.erase(it); Changed(); return MMS_OK;
    }
    auto command = ByHandle(commands, handle);
    if (command != commands.end()) {
        if (String(command->second->desc.info.ownerPlugin) != c.id) return MMS_E_ARGUMENT;
        for (const auto& [id, e] : elements) if (String(e.desc.commandId) == command->first) return MMS_E_BUSY;
        commands.erase(command); Changed(); return MMS_OK;
    }
    auto property = ByHandle(properties, handle);
    if (property != properties.end()) {
        if (String(property->second->desc.info.ownerPlugin) != c.id) return MMS_E_ARGUMENT;
        for (const auto& [id, e] : elements) if (String(e.desc.propertyId) == property->first) return MMS_E_BUSY;
        properties.erase(property); Changed(); return MMS_OK;
    }
    return MMS_E_NOT_FOUND;
}
MmsResult UiRegistry::Snapshot(UiClient& c, uint32_t flags, MmsUiSnapshotV1* out) {
    auto check = Check(c); if (check) return check; if (!Valid(out) || flags) return MMS_E_ARGUMENT;
    auto lease = std::make_unique<Lease>(); lease->owner = c.id; lease->kind = 1;
    for (const auto& [id, r] : commands) { lease->commandOwners.push_back(r); auto d = r->desc.info; d.h = Header<MmsCommandInfoV1>(); lease->commands.push_back(d); }
    for (const auto& [id, r] : properties) { lease->propertyOwners.push_back(r); auto d = r->desc.info; d.h = Header<MmsPropertyInfoV1>(); lease->properties.push_back(d); }
    for (const auto& [id, r] : elements) { lease->elementOwners.push_back(r); lease->elements.push_back(r.desc); }
    const MmsHandle handle = next_++;
    MmsUiSnapshotV1 result{Header<MmsUiSnapshotV1>(), handle, revision_,
        {lease->commands.data(), lease->commands.size(), sizeof(MmsCommandInfoV1), 1},
        {lease->elements.data(), lease->elements.size(), sizeof(MmsUiElementV1), 1},
        {lease->properties.data(), lease->properties.size(), sizeof(MmsPropertyInfoV1), 1}};
    leases_.emplace(handle, std::move(lease)); *out = result; return MMS_OK;
}
void UiRegistry::Release(UiClient& c, MmsHandle h, unsigned kind) {
    if (Check(c)) return; auto it = leases_.find(h); if (it != leases_.end() && it->second->owner == c.id && it->second->kind == kind) leases_.erase(it);
}
MmsResult UiRegistry::State(UiClient& c, MmsString id, MmsObjectRef target, MmsCommandStateV1* out, MmsHandle* handle) {
    auto check = Check(c); if (check) return check; if (!Valid(out) || !handle || !Null(target)) return MMS_E_ARGUMENT;
    auto found = commands.find(String(id)); if (found == commands.end()) return MMS_E_NOT_FOUND;
    auto record = found->second; if (record->owner->stopping) return MMS_E_NOT_READY;
    MmsCommandStateV1 state{}; state.h = Header<MmsCommandStateV1>();
    Callback guard(callbackDepth_); auto result = record->desc.getState(record->desc.context, target, &state);
    if (result) return result; if (!Valid(&state) || state.enabled > 1 || state.checked > 1 || state.visible > 1 || state.reserved) return MMS_E_FAILED;
    auto lease = std::make_unique<Lease>(); lease->kind = 2; lease->owner = c.id; state.disabledReason = lease->arena.Copy(state.disabledReason); state.h = Header<MmsCommandStateV1>();
    auto h = state.disabledReason.bytes ? next_++ : 0; if (h) leases_.emplace(h, std::move(lease)); *out = state; *handle = h; return MMS_OK;
}
MmsResult UiRegistry::Invoke(UiClient& c, MmsString id, MmsObjectRef target, MmsSpan args) {
    auto check = Check(c, true); if (check) return check; auto found = commands.find(String(id)); if (found == commands.end()) return MMS_E_NOT_FOUND;
    auto record = found->second;
    if (record->owner->stopping) return MMS_E_NOT_READY;
    if (!Null(target)) return MMS_E_ARGUMENT;
    if (args.count && (!args.data || args.count > 4096 || args.stride < sizeof(MmsNamedValue) || args.stride > 65536 || args.recordVersion != 1)) return MMS_E_ARGUMENT;
    std::map<std::string, const MmsArgumentV1*> schema;
    auto declared = record->desc.info.arguments;
    for (size_t i = 0; i < declared.count; ++i) { auto* a = reinterpret_cast<const MmsArgumentV1*>(static_cast<const unsigned char*>(declared.data) + i * declared.stride); schema.emplace(String(a->name), a); }
    Arena arena; std::vector<MmsNamedValue> values; std::set<std::string> supplied;
    for (size_t i = 0; i < args.count; ++i) {
        MmsNamedValue v; std::memcpy(&v, static_cast<const unsigned char*>(args.data) + i * args.stride, sizeof(v));
        auto name = String(v.name); auto s = schema.find(name);
        if (s == schema.end() || !supplied.insert(name).second || s->second->valueType != v.value.type || s->second->components != v.value.components) return MMS_E_ARGUMENT;
        v.name = arena.Copy(v.name); v.value = arena.Copy(v.value);
        if (!Fits(v.value, s->second->valueType, s->second->components, s->second->minimum, s->second->maximum, s->second->choices)) return MMS_E_ARGUMENT;
        values.push_back(v);
    }
    for (const auto& [name, s] : schema) if (!supplied.count(name)) {
        if (s->flags & MMS_ARGUMENT_REQUIRED) return MMS_E_ARGUMENT;
        values.push_back({arena.Copy(s->name), arena.Copy(s->defaultValue)});
    }
    MmsCommandStateV1 state{}; state.h = Header<MmsCommandStateV1>(); MmsHandle lease = 0;
    auto result = State(c, id, target, &state, &lease); Release(c, lease, 2);
    if (result) return result; if (!state.enabled || !state.visible) return MMS_E_NOT_READY;
    Callback guard(callbackDepth_);
    return record->desc.invoke(record->desc.context, target, {values.data(), values.size(), sizeof(MmsNamedValue), 1}, 0);
}
MmsResult UiRegistry::Read(UiClient& c, MmsString id, MmsObjectRef target, MmsValue* out, MmsHandle* handle) {
    auto check = Check(c); if (check) return check; if (!out || !handle || !Null(target)) return MMS_E_ARGUMENT;
    auto found = properties.find(String(id)); if (found == properties.end()) return MMS_E_NOT_FOUND; auto record = found->second;
    if (record->owner->stopping) return MMS_E_NOT_READY;
    Callback guard(callbackDepth_); MmsValue value{}; MmsHandle providerLease = 0;
    auto result = record->desc.read(record->desc.context, target, &value, &providerLease);
    struct ReleaseProvider { MmsPropertyDescV1& d; MmsHandle h; ~ReleaseProvider() { if (h) try { d.releaseValue(d.context, h); } catch (...) {} } } release{record->desc, providerLease};
    if (result) return result;
    if (value.type != record->desc.info.valueType || value.components != record->desc.info.components) return MMS_E_FAILED;
    auto lease = std::make_unique<Lease>(); lease->owner = c.id; lease->kind = 2; value = lease->arena.Copy(value);
    auto h = next_++; leases_.emplace(h, std::move(lease)); *out = value; *handle = h; return MMS_OK;
}
MmsResult UiRegistry::Begin(UiClient& c, uint64_t revision, MmsHandle* out) {
    auto check = Check(c, true); if (check) return check; if (!out) return MMS_E_ARGUMENT; if (revision != revision_) return MMS_E_STALE;
    auto t = std::make_unique<Layout>(); t->owner = c.id; t->revision = revision; auto h = next_++; layouts_.emplace(h, std::move(t)); *out = h; return MMS_OK;
}
MmsResult UiRegistry::Apply(UiClient& c, MmsHandle h, const MmsLayoutOpV1* op) {
    auto check = Check(c, true); if (check) return check; if (!Valid(op) || op->flags) return MMS_E_ARGUMENT;
    auto found = layouts_.find(h); if (found == layouts_.end() || found->second->owner != c.id) return MMS_E_NOT_FOUND;
    if (op->operation < 1 || op->operation > 8) return MMS_E_ARGUMENT;
    if (op->operation == MMS_LAYOUT_DOCK) return MMS_E_UNSUPPORTED;
    if (found->second->operations.size() >= 4096) return MMS_E_ARGUMENT;
    auto copy = *op; auto& t = *found->second; copy.elementId = t.arena.Copy(copy.elementId); copy.parentId = t.arena.Copy(copy.parentId); copy.beforeId = t.arena.Copy(copy.beforeId);
    if (op->operation == MMS_LAYOUT_CREATE_GROUP) copy.value.groupLabel = t.arena.Copy(copy.value.groupLabel);
    t.operations.push_back(copy); return MMS_OK;
}
MmsResult UiRegistry::Commit(UiClient& c, MmsHandle h) {
    auto check = Check(c, true); if (check) return check; auto found = layouts_.find(h);
    if (found == layouts_.end() || found->second->owner != c.id) return MMS_E_NOT_FOUND;
    auto transaction = std::move(found->second); layouts_.erase(found); if (transaction->revision != revision_) return MMS_E_STALE;
    auto tree = elements;
    auto clone = [](ElementRecord& e) {
        auto arena = std::make_shared<Arena>(); auto& d = e.desc;
        d.id = arena->Copy(d.id); d.ownerPlugin = arena->Copy(d.ownerPlugin); d.label = arena->Copy(d.label);
        d.parentId = arena->Copy(d.parentId); d.commandId = arena->Copy(d.commandId); d.propertyId = arena->Copy(d.propertyId); d.surfaceId = arena->Copy(d.surfaceId);
        e.arena = std::move(arena);
    };
    auto reset = [&](ElementRecord& e) { clone(e); e.desc.parentId = e.arena->Copy(Text(e.defaultParent)); e.visible = true; e.width = e.height = 0; e.key = e.modifiers = e.scope = 0; e.order = e.handle; e.userHints = false; e.effectiveHints = e.defaultHints; };
    auto insertBefore = [&](const std::string& id, MmsString before) -> MmsResult {
        auto& e = tree.at(id); const auto parent = String(e.desc.parentId), sibling = String(before);
        if (!sibling.empty()) { auto at = tree.find(sibling); if (at == tree.end() || sibling == id || String(at->second.desc.parentId) != parent) return MMS_E_ARGUMENT; }
        std::vector<std::string> children;
        for (const auto& [key, item] : tree) if (key != id && String(item.desc.parentId) == parent) children.push_back(key);
        std::stable_sort(children.begin(), children.end(), [&](const auto& a, const auto& b) { return tree.at(a).order < tree.at(b).order; });
        children.insert(sibling.empty() ? children.end() : std::find(children.begin(), children.end(), sibling), id);
        uint64_t order = 0; for (const auto& key : children) { auto& item = tree.at(key); item.order = ++order; item.effectiveHints.order = int32_t(order); item.userHints = true; }
        return MMS_OK;
    };
    for (const auto& op : transaction->operations) {
        const auto id = String(op.elementId);
        if (op.operation == MMS_LAYOUT_CREATE_GROUP) {
            if (op.expectedGeneration || !Own(c, op.elementId, Text(c.id))) return MMS_E_ARGUMENT;
            if (Exists(id) || tree.count(id)) return MMS_E_CONFLICT;
            ElementRecord e; e.handle = next_++; e.arena = std::make_shared<Arena>(); e.layoutGroup = true;
            e.desc.h = Header<MmsUiElementV1>(); e.desc.kind = MMS_UI_GROUP;
            e.desc.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_CONTAINER | MMS_UI_LAYOUT;
            e.defaultHints = e.effectiveHints = InitialHints();
            e.desc.id = e.arena->Copy(op.elementId); e.desc.ownerPlugin = e.arena->Copy(Text(c.id));
            e.desc.parentId = e.arena->Copy(op.parentId); e.desc.label = e.arena->Copy(op.value.groupLabel);
            e.defaultParent = String(op.parentId); e.order = e.handle;
            if (!Allowed(e.desc, tree)) return MMS_E_ARGUMENT;
            tree.emplace(id, std::move(e)); auto result = insertBefore(id, op.beforeId); if (result) return result;
            continue;
        }
        if (op.operation == MMS_LAYOUT_RESTORE && id.empty()) { if (op.expectedGeneration) return MMS_E_ARGUMENT; for (auto& [key, e] : tree) if (String(e.desc.ownerPlugin) != "mms") reset(e); continue; }
        auto it = tree.find(id); if (it == tree.end()) return MMS_E_NOT_FOUND;
        auto original = elements.find(id); if (original == elements.end() || original->second.desc.generation != op.expectedGeneration) return MMS_E_STALE;
        auto& e = it->second;
        if (String(e.desc.ownerPlugin) == "mms") return MMS_E_UNSUPPORTED;
        switch (op.operation) {
        case MMS_LAYOUT_MOVE: {
            if (!(e.desc.capabilities & MMS_UI_MOVE)) return MMS_E_UNSUPPORTED;
            // New strings belong to a fresh arena; the original snapshots retain theirs.
            auto arena = std::make_shared<Arena>(); arena->strings = e.arena->strings;
            e.desc.parentId = arena->Copy(op.parentId);
            // Keep the original arena alive through the transaction lease until
            // commit; copy the remaining strings into the new owner as well.
            e.desc.id = arena->Copy(e.desc.id); e.desc.ownerPlugin = arena->Copy(e.desc.ownerPlugin); e.desc.label = arena->Copy(e.desc.label);
            e.desc.commandId = arena->Copy(e.desc.commandId); e.desc.propertyId = arena->Copy(e.desc.propertyId); e.desc.surfaceId = arena->Copy(e.desc.surfaceId); e.arena = arena;
            if (!Allowed(e.desc, tree)) return MMS_E_ARGUMENT;
            auto result = insertBefore(id, op.beforeId); if (result) return result; break;
        }
        case MMS_LAYOUT_REMOVE_GROUP:
            if (!e.layoutGroup || String(e.desc.ownerPlugin) != c.id) return MMS_E_ARGUMENT;
            for (const auto& [key, child] : tree) if (String(child.desc.parentId) == id) return MMS_E_BUSY;
            tree.erase(it); break;
        case MMS_LAYOUT_VISIBLE: if (!(e.desc.capabilities & MMS_UI_HIDE)) return MMS_E_UNSUPPORTED; if (op.value.visible > 1) return MMS_E_ARGUMENT; e.visible = op.value.visible != 0; break;
        case MMS_LAYOUT_SIZE:
            if (!(e.desc.capabilities & MMS_UI_RESIZE)) return MMS_E_UNSUPPORTED;
            if (!std::isfinite(op.value.size.width) || !std::isfinite(op.value.size.height) || op.value.size.width <= 0 || op.value.size.height <= 0 || op.value.size.width > 32768 || op.value.size.height > 32768) return MMS_E_ARGUMENT;
            if (op.value.size.width < e.effectiveHints.minimum[0] || op.value.size.height < e.effectiveHints.minimum[1]) return MMS_E_ARGUMENT;
            e.width = op.value.size.width; e.height = op.value.size.height; break;
        case MMS_LAYOUT_SHORTCUT:
            if (!(e.desc.capabilities & MMS_UI_SHORTCUT) || !e.desc.commandId.bytes) return MMS_E_UNSUPPORTED;
            if (op.value.shortcut.virtualKey > 255 || op.value.shortcut.modifiers > 7 || op.value.shortcut.scope > 1 || op.value.shortcut.reserved) return MMS_E_ARGUMENT;
            e.key = op.value.shortcut.virtualKey; e.modifiers = op.value.shortcut.modifiers; e.scope = op.value.shortcut.scope; break;
        case MMS_LAYOUT_RESTORE: reset(e); break;
        default: return MMS_E_UNSUPPORTED;
        }
    }
    for (const auto& hint : transaction->hints) {
        auto original = elements.find(hint.id), it = tree.find(hint.id);
        if (original == elements.end() || it == tree.end() || original->second.desc.generation != hint.generation) return MMS_E_STALE;
        if (!(it->second.desc.capabilities & MMS_UI_LAYOUT)) return MMS_E_UNSUPPORTED;
        it->second.effectiveHints = hint.value; it->second.userHints = true;
    }
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> shortcuts;
    for (const auto& [id, e] : tree) {
        if (String(e.desc.ownerPlugin) != "mms" && !Allowed(e.desc, tree)) return MMS_E_ARGUMENT;
        if (e.key && !shortcuts.emplace(e.key, e.modifiers, e.scope).second) return MMS_E_CONFLICT;
    }
    for (auto& [id, e] : tree) e.desc.generation = generation_++;
    elements.swap(tree); Changed(); return MMS_OK;
}
bool UiRegistry::HasLeases(const UiClient& c) const { if (c.unsafeToUnload) return true; for (const auto& [h, lease] : leases_) if (lease->owner == c.id) return true; return false; }
void UiRegistry::Retire(UiClient& c) {
    for (auto it = layouts_.begin(); it != layouts_.end();) if (it->second->owner == c.id) it = layouts_.erase(it); else ++it;
    for (auto it = elements.begin(); it != elements.end();) if (String(it->second.desc.ownerPlugin) == c.id) { DestroyView(it->first); it = elements.erase(it); } else ++it;
    for (auto it = commands.begin(); it != commands.end();) if (String(it->second->desc.info.ownerPlugin) == c.id) it = commands.erase(it); else ++it;
    for (auto it = properties.begin(); it != properties.end();) if (String(it->second->desc.info.ownerPlugin) == c.id) it = properties.erase(it); else ++it;
    for (auto it = surfaces.begin(); it != surfaces.end();) if (it->second->owner == &c) it = surfaces.erase(it); else ++it;
    Changed();
}
MmsUiApiV1 UiRegistry::Api(UiClient& client) {
    // Each context carries its own owner; no plugin can forge another owner by
    // filling in a public descriptor. Exceptions never cross the C boundary.
    MmsUiApiV1 a{}; a.h = Header<MmsUiApiV1>(); a.context = &client;
#define UI_BEGIN auto& c = *static_cast<UiClient*>(p); auto& r = *c.registry; return Boundary([&]() -> MmsResult
    a.registerCommand = [](void* p, const MmsCommandDescV1* d, MmsHandle* h) { UI_BEGIN { return r.Command(c, d, h); }); };
    a.registerElement = [](void* p, const MmsUiElementV1* d, MmsHandle* h) { UI_BEGIN { return r.Element(c, d, h); }); };
    a.registerProperty = [](void* p, const MmsPropertyDescV1* d, MmsHandle* h) { UI_BEGIN { return r.Property(c, d, h); }); };
    a.updateCommand = [](void* p, MmsHandle h, const MmsCommandDescV1* d) { UI_BEGIN { MmsHandle out; return r.Command(c, d, &out, h); }); };
    a.updateElement = [](void* p, MmsHandle h, const MmsUiElementV1* d) { UI_BEGIN { MmsHandle out; return r.Element(c, d, &out, h); }); };
    a.updateProperty = [](void* p, MmsHandle h, const MmsPropertyDescV1* d) { UI_BEGIN { MmsHandle out; return r.Property(c, d, &out, h); }); };
    a.unregisterItem = [](void* p, MmsHandle h) { UI_BEGIN { return r.Remove(c, h); }); };
    a.acquireSnapshot = [](void* p, uint32_t f, MmsUiSnapshotV1* out) { UI_BEGIN { return r.Snapshot(c, f, out); }); };
    a.releaseSnapshot = [](void* p, MmsHandle h) { auto& c = *static_cast<UiClient*>(p); c.registry->Release(c, h, 1); };
    a.getCommandState = [](void* p, MmsString id, MmsObjectRef t, MmsCommandStateV1* out, MmsHandle* h) { UI_BEGIN { return r.State(c, id, t, out, h); }); };
    a.invoke = [](void* p, MmsString id, MmsObjectRef t, MmsSpan v) { UI_BEGIN { return r.Invoke(c, id, t, v); }); };
    a.readProperty = [](void* p, MmsString id, MmsObjectRef t, MmsValue* out, MmsHandle* h) { UI_BEGIN { return r.Read(c, id, t, out, h); }); };
    a.releaseValue = [](void* p, MmsHandle h) { auto& c = *static_cast<UiClient*>(p); c.registry->Release(c, h, 2); };
    a.stageProperty = [](void* p, MmsHandle, MmsString, MmsObjectRef, const MmsValue*) { UI_BEGIN { auto result = r.Check(c, true); return result ? result : MMS_E_NOT_READY; }); };
    a.beginLayout = [](void* p, uint64_t rev, MmsHandle* out) { UI_BEGIN { return r.Begin(c, rev, out); }); };
    a.applyLayout = [](void* p, MmsHandle h, const MmsLayoutOpV1* op) { UI_BEGIN { return r.Apply(c, h, op); }); };
    a.commitLayout = [](void* p, MmsHandle h) { UI_BEGIN { return r.Commit(c, h); }); };
    a.cancelLayout = [](void* p, MmsHandle h) { auto& c = *static_cast<UiClient*>(p); auto& r = *c.registry; if (r.Check(c)) return; auto it = r.layouts_.find(h); if (it != r.layouts_.end() && it->second->owner == c.id) r.layouts_.erase(it); };
#undef UI_BEGIN
    return a;
}
MmsResult UiRegistry::DefaultHints(UiClient& c, MmsString id, const MmsUiLayoutHintsV1* hints) {
    auto check = Check(c, true); if (check) return check; check = ValidateHints(hints); if (check) return check;
    auto it = elements.find(String(id)); if (it == elements.end()) return MMS_E_NOT_FOUND;
    if (String(it->second.desc.ownerPlugin) != c.id) return MMS_E_ARGUMENT;
    if (!(it->second.desc.capabilities & MMS_UI_LAYOUT)) return MMS_E_UNSUPPORTED;
    it->second.defaultHints = *hints; it->second.defaultHints.h = Header<MmsUiLayoutHintsV1>();
    if (!it->second.userHints) it->second.effectiveHints = it->second.defaultHints;
    it->second.desc.generation = generation_++; Changed(); return MMS_OK;
}
MmsResult UiRegistry::StageHints(UiClient& c, MmsHandle h, MmsString id, uint64_t generation, const MmsUiLayoutHintsV1* hints) {
    auto check = Check(c, true); if (check) return check; check = ValidateHints(hints); if (check) return check;
    auto t = layouts_.find(h); if (t == layouts_.end() || t->second->owner != c.id) return MMS_E_NOT_FOUND;
    if (t->second->hints.size() >= 4096) return MMS_E_ARGUMENT;
    auto value = *hints; value.h = Header<MmsUiLayoutHintsV1>(); t->second->hints.push_back({String(id), generation, value}); return MMS_OK;
}
bool UiRegistry::PublishGeometry(const std::map<std::string, Geometry>& input, uint64_t revision) {
    if (GetCurrentThreadId() != thread_ || revision != revision_) return false;
    std::map<std::string, MmsUiLayoutItemV1> candidate;
    for (const auto& [id, e] : elements) if (e.desc.capabilities & MMS_UI_LAYOUT) {
        auto it = input.find(id); if (it == input.end()) return false;
        const auto& g = it->second;
        for (float v : {g.x, g.y, g.width, g.height}) if (!std::isfinite(v)) return false;
        if (g.width < 0 || g.height < 0) return false;
        MmsUiLayoutItemV1 item{}; item.h = Header<MmsUiLayoutItemV1>(); item.elementId = e.desc.id; item.generation = e.desc.generation;
        item.defaults = e.defaultHints; item.effective = e.effectiveHints;
        item.rect[0] = g.x; item.rect[1] = g.y; item.rect[2] = g.width; item.rect[3] = g.height; item.visible = g.visible; item.overflowed = g.overflowed;
        candidate.emplace(id, item);
    }
    bool changed = geometryUiRevision_ != revision || candidate.size() != geometry_.size();
    if (!changed) for (const auto& [id, value] : candidate) {
        auto prior = geometry_.find(id);
        if (prior == geometry_.end() || std::memcmp(value.rect, prior->second.rect, sizeof(value.rect)) || value.visible != prior->second.visible || value.overflowed != prior->second.overflowed) { changed = true; break; }
    }
    geometry_.swap(candidate); geometryUiRevision_ = revision; if (changed) ++geometryRevision_; return true;
}
MmsResult UiRegistry::LayoutSnapshot(UiClient& c, MmsUiLayoutSnapshotV1* out) {
    auto check = Check(c); if (check) return check; if (!Valid(out)) return MMS_E_ARGUMENT;
    if (geometryUiRevision_ != revision_) return MMS_E_NOT_READY;
    auto lease = std::make_unique<Lease>(); lease->owner = c.id; lease->kind = 4;
    for (const auto& [id, item] : geometry_) { auto copy = item; copy.elementId = lease->arena.Copy(Text(id)); lease->geometry.push_back(copy); }
    MmsHandle h = next_++; MmsUiLayoutSnapshotV1 result{Header<MmsUiLayoutSnapshotV1>(), h, revision_, geometryRevision_, {lease->geometry.data(), lease->geometry.size(), sizeof(MmsUiLayoutItemV1), 1}};
    leases_.emplace(h, std::move(lease)); *out = result; return MMS_OK;
}
MmsUiLayoutApiV1 UiRegistry::LayoutApi(UiClient& client) {
    MmsUiLayoutApiV1 a{}; a.h = Header<MmsUiLayoutApiV1>(); a.context = &client;
    a.setDefaultHints = [](void* p, MmsString id, const MmsUiLayoutHintsV1* h) { return Boundary([&] { auto& c = *static_cast<UiClient*>(p); return c.registry->DefaultHints(c, id, h); }); };
    a.stageHints = [](void* p, MmsHandle t, MmsString id, uint64_t g, const MmsUiLayoutHintsV1* h) { return Boundary([&] { auto& c = *static_cast<UiClient*>(p); return c.registry->StageHints(c, t, id, g, h); }); };
    a.acquireSnapshot = [](void* p, MmsUiLayoutSnapshotV1* out) { return Boundary([&] { auto& c = *static_cast<UiClient*>(p); return c.registry->LayoutSnapshot(c, out); }); };
    a.releaseSnapshot = [](void* p, MmsHandle h) { auto& c = *static_cast<UiClient*>(p); c.registry->Release(c, h, 4); };
    return a;
}
MmsUiSurfaceApiV1 UiRegistry::SurfaceApi(UiClient& client) {
    MmsUiSurfaceApiV1 a{}; a.h = Header<MmsUiSurfaceApiV1>(); a.context = &client;
    a.acquireAnchors = [](void* p, MmsSpan* out, MmsHandle* handle) { return Boundary([&]() -> MmsResult {
        auto& c = *static_cast<UiClient*>(p); auto& r = *c.registry; auto check = r.Check(c); if (check) return check; if (!out || !handle) return MMS_E_ARGUMENT;
        auto lease = std::make_unique<Lease>(); lease->owner = c.id; lease->kind = 3;
        for (const auto& [id, e] : r.elements) if (String(e.desc.ownerPlugin) == "mms") {
            MmsUiAnchorV1 anchor{}; anchor.h = Header<MmsUiAnchorV1>(); anchor.id = lease->arena.Copy(e.desc.id); anchor.label = lease->arena.Copy(e.desc.label);
            anchor.allowedKinds = id == "mms.menu.plugins" ? (1ull << MMS_UI_MENU) | (1ull << MMS_UI_MENU_ITEM) | (1ull << MMS_UI_SEPARATOR) : id == "mms.workspace.tabs.extensions" ? (1ull << MMS_UI_PANEL) : (1ull << MMS_UI_BUTTON) | (1ull << MMS_UI_SEPARATOR) | (1ull << MMS_UI_GROUP);
            if (id == "mms.workspace.tabs.extensions") anchor.flags = MMS_ANCHOR_NATIVE_CHILD;
            lease->anchors.push_back(anchor);
        }
        MmsSpan span{lease->anchors.data(), lease->anchors.size(), sizeof(MmsUiAnchorV1), 1}; auto h = r.next_++; r.leases_.emplace(h, std::move(lease)); *out = span; *handle = h; return MMS_OK;
    }); };
    a.releaseAnchors = [](void* p, MmsHandle h) { auto& c = *static_cast<UiClient*>(p); c.registry->Release(c, h, 3); };
    a.registerSurface = [](void* p, const MmsUiSurfaceDescV1* d, MmsHandle* out) { return Boundary([&] { auto& c = *static_cast<UiClient*>(p); return c.registry->Surface(c, d, out); }); };
    a.unregisterSurface = [](void* p, MmsHandle h) { return Boundary([&] { auto& c = *static_cast<UiClient*>(p); return c.registry->RemoveSurface(c, h); }); };
    return a;
}
MmsResult UiRegistry::Surface(UiClient& c, const MmsUiSurfaceDescV1* d, MmsHandle* out) {
    auto check = Check(c, true); if (check) return check;
    if (!Valid(d) || !out || !Own(c, d->id, Text(c.id)) || d->flags & ~MMS_SURFACE_MULTIPLE_VIEWS || d->reserved ||
        !d->createView || !d->resizeView || !d->showView || !d->focusView || !d->destroyView) return MMS_E_ARGUMENT;
    for (auto size : {d->minimumWidth, d->minimumHeight, d->preferredWidth, d->preferredHeight})
        if (!std::isfinite(size) || size < 0 || size > 32768) return MMS_E_ARGUMENT;
    if ((d->preferredWidth && d->preferredWidth < d->minimumWidth) || (d->preferredHeight && d->preferredHeight < d->minimumHeight)) return MMS_E_ARGUMENT;
    auto id = String(d->id); if (surfaces.count(id) || Exists(id)) return MMS_E_CONFLICT;
    auto r = std::make_shared<SurfaceRecord>(); r->owner = &c; r->handle = next_++; r->arena = std::make_shared<Arena>(); r->desc = *d; r->desc.h = Header<MmsUiSurfaceDescV1>(); r->desc.id = r->arena->Copy(d->id);
    surfaces.emplace(id, r); *out = r->handle; Changed(); return MMS_OK;
}
MmsResult UiRegistry::RemoveSurface(UiClient& c, MmsHandle h) {
    auto check = Check(c); if (check) return check; if (callbackDepth_) return MMS_E_BUSY;
    auto it = ByHandle(surfaces, h); if (it == surfaces.end()) return MMS_E_NOT_FOUND; if (it->second->owner != &c) return MMS_E_ARGUMENT;
    for (const auto& [id, e] : elements) if (String(e.desc.surfaceId) == it->first) return MMS_E_BUSY;
    surfaces.erase(it); Changed(); return MMS_OK;
}
void UiRegistry::DestroyView(const std::string& element) {
    auto found = views_.find(element); if (found == views_.end()) return;
    auto view = std::move(found->second);
    views_.erase(found);
    Callback guard(callbackDepth_);
    // A throwing plugin cannot leave an entry that will be destroyed twice.
    auto result = Boundary([&] { view->surface->desc.destroyView(view->context); return MMS_OK; });
    if (result != MMS_OK || IsWindow(view->child)) view->surface->owner->unsafeToUnload = true;
}
void UiRegistry::DestroyViews() { while (!views_.empty()) DestroyView(views_.begin()->first); }
MmsResult UiRegistry::ShowSurface(const std::string& id, HWND parent, const MmsUiRectV1& bounds, uint32_t dpi, bool visible) {
    return Boundary([&]() -> MmsResult {
        if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
        auto element = elements.find(id); if (element == elements.end() || element->second.desc.kind != MMS_UI_PANEL) return MMS_E_NOT_FOUND;
        auto surface = surfaces.find(String(element->second.desc.surfaceId)); if (surface == surfaces.end()) return MMS_E_NOT_FOUND;
        if (surface->second->owner->stopping) return MMS_E_NOT_READY;
        if (!IsWindow(parent) || GetWindowThreadProcessId(parent, nullptr) != thread_ || dpi < 48 || dpi > 960) return MMS_E_ARGUMENT;
        auto found = views_.find(id);
        if (found != views_.end() && found->second->parent != parent) { DestroyView(id); found = views_.end(); }
        if (found == views_.end()) {
            if (!visible) return MMS_OK;
            auto v = std::make_unique<View>(); v->surface = surface->second; v->parent = parent; v->bounds = bounds; v->dpi = dpi;
            MmsUiMountV1 mount{Header<MmsUiMountV1>(), Text(id), reinterpret_cast<uintptr_t>(parent), bounds, dpi, 0}; uintptr_t child = 0;
            // Allocate the map node before invoking plugin code. Once create
            // succeeds no allocation can strand a live view outside the map.
            auto inserted = views_.emplace(id, std::move(v)).first;
            auto& created = *inserted->second;
            Callback guard(callbackDepth_);
            auto r = Boundary([&] { return created.surface->desc.createView(created.surface->desc.context, &mount, &created.context, &child); });
            created.child = reinterpret_cast<HWND>(child);
            if (r != MMS_OK) { views_.erase(inserted); return r; }
            if (!created.context || !IsWindow(created.child) || GetParent(created.child) != parent || !(GetWindowLongPtrW(created.child, GWL_STYLE) & WS_CHILD) || GetWindowThreadProcessId(created.child, nullptr) != thread_) {
                if (created.context) DestroyView(id); else views_.erase(inserted);
                return MMS_E_FAILED;
            }
            found = inserted;
        }
        auto& v = *found->second; Callback guard(callbackDepth_);
        if (std::memcmp(&v.bounds, &bounds, sizeof(bounds)) || v.dpi != dpi) { v.surface->desc.resizeView(v.context, &bounds, dpi); v.bounds = bounds; v.dpi = dpi; }
        if (v.visible != visible) { v.surface->desc.showView(v.context, visible ? 1 : 0); v.visible = visible; }
        return MMS_OK;
    });
}
MmsResult UiRegistry::FocusSurface(const std::string& id) {
    return Boundary([&]() -> MmsResult { if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
        auto found = views_.find(id); if (found == views_.end() || !found->second->visible) return MMS_E_NOT_READY; if (found->second->surface->owner->stopping) return MMS_E_NOT_READY;
        Callback guard(callbackDepth_); return found->second->surface->desc.focusView(found->second->context);
    });
}
}
