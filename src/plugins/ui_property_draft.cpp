#include "ui_property_draft.hpp"
#include <new>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
bool ValidShape(const MmsValue& value) noexcept {
    if (value.type < MMS_VALUE_BOOL || value.type > MMS_VALUE_BYTES) return false;
    return value.type == MMS_VALUE_VECTOR ? value.components >= 1 && value.components <= 4 : value.components == 0;
}

template<class F> MmsResult Attempt(F&& operation) noexcept {
    try { return operation(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_ARGUMENT; }
}
}

MmsResult UiPropertyDraft::Begin(const MmsValue& canonical, uint64_t sourceRevision) noexcept {
    if (!sourceRevision || !ValidShape(canonical)) return MMS_E_ARGUMENT;
    // Reject known oversized representations before copying/formatting a
    // multi-megabyte binary value. The codec's own transfer limit is unchanged.
    if ((canonical.type == MMS_VALUE_TEXT && canonical.value.text.bytes > kMaxFieldBytes) ||
        (canonical.type == MMS_VALUE_BYTES && canonical.value.bytes.bytes > kMaxFieldBytes / 2)) return MMS_E_ARGUMENT;
    return Attempt([&]() -> MmsResult {
        UiPropertyValue baseline;
        if (const auto result = UiPropertyValue::Copy(canonical, baseline)) return result;
        std::vector<std::string> initial;
        if (const auto result = baseline.Format(initial)) return result;
        for (const auto& field : initial) if (field.size() > kMaxFieldBytes) return MMS_E_ARGUMENT;
        auto fields = initial;
        // All fallible work is complete. Failed Begin leaves the previous
        // session, original fields and IME state untouched.
        baseline_ = std::move(baseline);
        initialFields_.swap(initial);
        fields_.swap(fields);
        sourceRevision_ = sourceRevision;
        composing_ = false;
        return MMS_OK;
    });
}

MmsResult UiPropertyDraft::SetField(size_t index, std::string_view text) noexcept {
    if (!Active()) return MMS_E_NOT_READY;
    if (index >= fields_.size() || text.size() > kMaxFieldBytes || (text.size() && !text.data())) return MMS_E_ARGUMENT;
    return Attempt([&]() -> MmsResult {
        std::string field;
        if (!text.empty()) field.assign(text.data(), text.size());
        fields_[index].swap(field);
        return MMS_OK;
    });
}

void UiPropertyDraft::SetComposing(bool composing) noexcept {
    composing_ = Active() && composing;
}

MmsResult UiPropertyDraft::Prepare(const MmsValue& current, uint64_t sourceRevision,
    UiPropertyValue& output, bool& changed) const noexcept {
    if (!Active()) return MMS_E_NOT_READY;
    if (composing_) return MMS_E_BUSY;
    if (sourceRevision != sourceRevision_ || !ValidShape(current)) return MMS_E_STALE;
    return Attempt([&]() -> MmsResult {
        UiPropertyValue canonical;
        if (const auto result = UiPropertyValue::Copy(current, canonical))
            return result == MMS_E_MEMORY ? result : MMS_E_STALE;
        if (!canonical.Equals(baseline_)) return MMS_E_STALE;
        std::vector<std::string_view> fields;
        fields.reserve(fields_.size());
        for (const auto& field : fields_) fields.emplace_back(field);
        const auto schema = baseline_.View();
        UiPropertyValue candidate;
        if (const auto result = UiPropertyValue::Parse(schema.type, schema.components, fields, candidate)) return result;
        const bool different = !candidate.Equals(baseline_);
        output = std::move(candidate);
        changed = different;
        return MMS_OK;
    });
}

void UiPropertyDraft::Cancel() noexcept {
    baseline_ = UiPropertyValue{};
    initialFields_.clear();
    fields_.clear();
    sourceRevision_ = 0;
    composing_ = false;
}

}
