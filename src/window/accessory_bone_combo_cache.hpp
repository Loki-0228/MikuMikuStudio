#pragma once
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ui_language.hpp"
#include <commctrl.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace mikudancestudio::accessory_bone_combo {
inline bool Listed(const mdl::BoneRecord& bone) noexcept {
    return bone.type < mdl::BoneType::InertTip || bone.type == mdl::BoneType::FixedAxis;
}

// This optional cache belongs to one native window. It compares the actual
// source bytes on every refresh, so edits do not need a metadata revision API.
// Larger inputs keep the ordinary authoritative label comparison instead.
struct Source {
    struct Bone {
        std::uint32_t index = 0;
        std::array<char, sizeof(mdl::BoneRecord::name)> native{};
        std::array<char, sizeof(mdl::BoneRecord::nameEn)> english{};
        std::wstring nativeWide, englishWide;
    };
    const mdl::ModelRecord* model = nullptr;
    const mdl::BoneRecord* table = nullptr;
    std::uint32_t count = 0;
    bool english = false, translate = false;
    std::vector<Bone> bones;

    static bool Capture(const mdl::ModelRecord* model, bool english, Source& output) noexcept {
        try {
            constexpr std::size_t maxBones = 16384, maxWideUnits = 262144;
            Source candidate; candidate.model = model; candidate.english = english;
            candidate.translate = gModelNameAutoTranslation;
            if (model) { candidate.table = model->boneTable; candidate.count = model->boneCount; }
            if (candidate.count > maxBones) return false;
            std::size_t remaining = maxWideUnits;
            const auto copy = [&](const wchar_t* input, std::wstring& target) {
                if (!input) return true;
                const auto length = wcsnlen_s(input, remaining + 1);
                if (length > remaining) return false;
                target.assign(input, length); remaining -= length; return true;
            };
            if (candidate.table) {
                candidate.bones.reserve(candidate.count);
                for (std::uint32_t i = 0; i < candidate.count; ++i) {
                    const auto& record = candidate.table[i]; if (!Listed(record)) continue;
                    Bone bone; bone.index = i;
                    std::memcpy(bone.native.data(), record.name, bone.native.size());
                    std::memcpy(bone.english.data(), record.nameEn, bone.english.size());
                    if (!copy(record.jpText, bone.nativeWide) || !copy(record.enText, bone.englishWide)) return false;
                    candidate.bones.push_back(std::move(bone));
                }
            }
            output = std::move(candidate); return true;
        } catch (...) { return false; } // Cache preparation is optional.
    }
    bool Matches(const mdl::ModelRecord* current, bool useEnglish) const noexcept {
        if (model != current || english != useEnglish || translate != bool(gModelNameAutoTranslation)) return false;
        if (current && (current->boneTable != table || current->boneCount != count)) return false;
        std::size_t row = 0;
        if (table) for (std::uint32_t i = 0; i < count; ++i) {
            const auto& record = table[i]; if (!Listed(record)) continue;
            if (row == bones.size()) return false;
            const auto& saved = bones[row++];
            if (saved.index != i || std::memcmp(saved.native.data(), record.name, saved.native.size()) ||
                std::memcmp(saved.english.data(), record.nameEn, saved.english.size()) ||
                saved.nativeWide != (record.jpText ? record.jpText : L"") ||
                saved.englishWide != (record.enText ? record.enText : L"")) return false;
        }
        return row == bones.size();
    }
};

struct State {
    HWND window = nullptr;
    bool alive = true, valid = false, busy = false, saturated = false;
    std::size_t reading = 0;
    std::uint64_t mutations = 0;
    Source source;
    void Invalidate() noexcept {
        valid = false;
        if (mutations != (std::numeric_limits<std::uint64_t>::max)()) ++mutations;
        else saturated = true;
    }
    void Remember(Source&& value, std::uint64_t observed) noexcept {
        if (alive && !saturated && observed == mutations) { source = std::move(value); valid = true; }
    }
};
using Cache = std::shared_ptr<State>;
struct Holder { Cache value; };
inline LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* holder = reinterpret_cast<Holder*>(data); const auto state = holder->value;
    switch (message) {
    case CB_RESETCONTENT: case CB_ADDSTRING: case CB_INSERTSTRING:
    case CB_DELETESTRING: case CB_SETITEMDATA: case CB_DIR:
        state->Invalidate();
        // CB_GETLBTEXT has no buffer-size argument. Do not allow a nested
        // SendMessage to grow or replace the row while a bounded read is in
        // flight; the caller sees failure and can retry after the read.
        if (state->reading) return CB_ERR;
        break;
    case WM_NCDESTROY:
        state->alive = state->valid = false;
        RemoveWindowSubclass(window, Observe, id); delete holder; break;
    default: break;
    }
    return DefSubclassProc(window, message, wp, lp);
}
inline Cache Find(HWND window, bool create = false) noexcept {
    if (!window || !IsWindow(window)) return {};
    const auto id = reinterpret_cast<UINT_PTR>(&Observe); DWORD_PTR data = 0;
    if (GetWindowSubclass(window, Observe, id, &data)) return reinterpret_cast<Holder*>(data)->value;
    if (!create) return {};
    try {
        auto holder = std::make_unique<Holder>(); holder->value = std::make_shared<State>(); holder->value->window = window;
        if (!SetWindowSubclass(window, Observe, id, reinterpret_cast<DWORD_PTR>(holder.get()))) return {};
        auto result = holder->value; holder.release(); return result;
    } catch (...) { return {}; }
}
struct Guard {
    Cache state;
    explicit Guard(Cache value) noexcept : state(std::move(value)) { if (state) state->busy = true; }
    ~Guard() { if (state) state->busy = false; }
    bool Alive() const noexcept { return state && state->alive; }
};
struct ReadGuard {
    Cache state;
    explicit ReadGuard(Cache value) noexcept : state(std::move(value)) { ++state->reading; }
    ~ReadGuard() { --state->reading; }
};
}
