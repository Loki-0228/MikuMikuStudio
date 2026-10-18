#include "native_accessory_settings.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include <array>

namespace mikudancestudio::plugins {
struct AccessorySettingsPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    using Name = std::array<char, sizeof(mdl::AccessoryRecord::name)>;
    struct Value { std::uint8_t order = 0; Name name{}; };
    struct Entry {
        unsigned slot;
        mdl::AccessoryRecord* object;
        std::shared_ptr<RetainedSceneObject> owner;
        MmsObjectRef initial;
        Value before, after;
        bool renamed = false, reordered = false;
    };
    MMDApp& app;
    std::vector<Entry> entries;
    int beforeSplit = 0, afterSplit = 0;
    bool read = false, first = true, splitChanged = false;
    explicit State(MMDApp& value) : app(value) {}
    bool Members() const noexcept {
        std::size_t index = 0;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.AccessorySlot(slot)) {
            if (index == entries.size()) return false;
            const auto& e = entries[index++];
            if (e.slot != slot || e.object != object || !e.owner->Present() || !SceneObjectPresent(object, e.owner->Id())) return false;
        }
        return index == entries.size();
    }
    bool Source(bool forward, bool initial) const noexcept {
        if (!Members()) return false;
        if ((initial || splitChanged) && app.AccessoryRenderSplitOrder() != (forward ? beforeSplit : afterSplit)) return false;
        for (const auto& e : entries) {
            const auto& value = forward ? e.before : e.after;
            if (e.object->order != value.order || ((initial || e.renamed) &&
                std::memcmp(e.object->name, value.name.data(), value.name.size()))) return false;
            if (initial && SceneObjectIdentity(e.object).generation != e.initial.generation) return false;
        }
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        std::vector<std::uint64_t> generations;
        bool forward;
        Candidate(std::shared_ptr<State> value, bool direction) : state(std::move(value)), forward(direction) {
            generations.reserve(state->entries.size());
            for (const auto& e : state->entries) generations.push_back(SceneObjectIdentity(e.object).generation);
        }
        bool IsCurrent() const noexcept override {
            if (!state->Source(forward, state->first)) return false;
            for (std::size_t i = 0; i < generations.size(); ++i)
                if (SceneObjectIdentity(state->entries[i].object).generation != generations[i]) return false;
            return true;
        }
        void Publish() noexcept override {
            for (const auto& e : state->entries) {
                const auto& target = forward ? e.after : e.before;
                if (e.reordered) e.object->order = target.order;
                if (e.renamed) std::memcpy(e.object->name, target.name.data(), target.name.size());
            }
            if (state->splitChanged) state->app.AccessoryRenderSplitOrder() = forward ? state->afterSplit : state->beforeSplit;
            state->first = false;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        return Boundary([&]() -> MmsResult {
            if (!read || !Source(forward, first)) return MMS_E_STALE;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return sizeof(State) + 4 * sizeof(void*) + entries.capacity() * sizeof(Entry);
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult AccessorySettingsPlan::Begin(MMDApp& app) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto state = std::make_shared<State>(app);
        std::array<bool, kModelSlotCount> used{};
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.AccessorySlot(slot)) {
            if (object->order >= kModelSlotCount || used[object->order] || !std::memchr(object->name, 0, sizeof object->name)) return MMS_E_STALE;
            used[object->order] = true;
            State::Entry entry{}; entry.slot = slot; entry.object = object;
            entry.owner = RetainSceneObject(app, MMS_OBJECT_ACCESSORY, slot); entry.initial = SceneObjectIdentity(object);
            entry.before.order = object->order;
            std::memcpy(entry.before.name.data(), object->name, sizeof object->name);
            entry.after = entry.before; state->entries.push_back(std::move(entry));
        }
        if (state->entries.empty()) return MMS_E_NOT_READY;
        for (std::size_t i = 0; i < state->entries.size(); ++i) if (!used[i]) return MMS_E_STALE;
        state->beforeSplit = state->afterSplit = app.AccessoryRenderSplitOrder();
        if (state->beforeSplit < 0 || state->beforeSplit > int(state->entries.size())) return MMS_E_STALE;
        state_ = std::move(state); return MMS_OK;
    });
}
MmsResult AccessorySettingsPlan::ReadDialog(HWND dialog) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    auto& state = *state_; state.read = false;
    return Boundary([&]() -> MmsResult {
        if (!state.Source(true, true)) return MMS_E_STALE;
        const auto list = GetDlgItem(dialog, panel::kOrderListBox);
        const auto count = SendMessageW(list, LB_GETCOUNT, 0, 0);
        if (!list || count != LRESULT(state.entries.size()) || !state.app.AccessoryOrderArray()) return MMS_E_ARGUMENT;
        BOOL valid = FALSE;
        const auto split = GetDlgItemInt(dialog, panel::kAccessoryOrderEdit, &valid, FALSE);
        if (!valid || split > state.entries.size()) return MMS_E_ARGUMENT;
        auto** order = static_cast<mdl::AccessoryRecord**>(state.app.AccessoryOrderArray());
        std::array<bool, kModelSlotCount> used{};
        for (int row = 0; row < count; ++row) {
            const auto entry = std::find_if(state.entries.begin(), state.entries.end(), [&](const auto& e) { return e.object == order[row]; });
            if (entry == state.entries.end() || used[entry->slot]) return MMS_E_ARGUMENT;
            used[entry->slot] = true;
            const auto length = SendMessageW(list, LB_GETTEXTLEN, row, 0);
            if (length < 0 || length >= 256) return MMS_E_ARGUMENT;
            std::wstring text(std::size_t(length) + 1, L'\0');
            if (SendMessageW(list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(text.data())) != length) return MMS_E_FAILED;
            entry->after = entry->before; entry->after.order = std::uint8_t(row);
            // Preserve the exact original bytes (including padding) when the
            // displayed name is unchanged. Reject lossy new names atomically.
            if (text_encoding::Display(entry->before.name.data()) != text.c_str()) {
                entry->after.name.fill(0);
                if (!text_encoding::EncodeDisplay(entry->after.name.data(), entry->after.name.size(), text.c_str())) return MMS_E_ARGUMENT;
            }
            entry->renamed = entry->after.name != entry->before.name;
            entry->reordered = entry->after.order != entry->before.order;
        }
        if (!state.Source(true, true)) return MMS_E_STALE;
        state.afterSplit = int(split); state.splitChanged = state.afterSplit != state.beforeSplit;
        state.read = true; return MMS_OK;
    });
}
MmsResult AccessorySettingsPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->read) return MMS_E_NOT_READY;
    if (!state_->Source(true, true)) return MMS_E_STALE;
    const bool changed = state_->splitChanged || std::any_of(state_->entries.begin(), state_->entries.end(),
        [](const auto& e) { return e.renamed || e.reordered; });
    if (changed) out = std::move(state_); else state_.reset();
    return MMS_OK;
}
}
#endif
