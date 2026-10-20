#include "native_model_names.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <cwchar>
#include <exception>

namespace mikumikustudio::plugins {
namespace {
thread_local std::uint64_t uiRevision = 0;
thread_local const MMDApp* publication = nullptr;
thread_local bool* closing = nullptr;
bool Ready(MMDApp& a, bool replay) noexcept {
    const bool authorized = replay && (publication == &a || NativeObjectPublicationAuthorized(&a));
    return (!closing || !*closing) && (!bake::UiLocked() || authorized) &&
        (!a.SceneMutationInProgress() || authorized) && !SceneSaveBlocksEdits() && !NativeObjectUiPending(&a) &&
        !GetCapture() && !a.PlaybackActive() && !a.FrameStepPlayback() && !a.state.recordPlaybackStartPending &&
        !a.RecordingWindow() && !a.FrameRangeDialog() && !a.state.frameCopyDialog && !a.GravitySettingDialog() &&
        !a.GroundShadowColorDialog() && !a.AccessoryOrderArray() && !a.state.accessoryEditDialogOpen &&
        !NativeModelOutlineDialogBlocks(a) && a.MainWindow() &&
        GetWindowThreadProcessId(a.MainWindow(), nullptr) == GetCurrentThreadId();
}
struct WindowGuard {
    std::array<HWND, 2> windows{};
    std::array<bool, 2> close{};
    bool requested = false;
    bool* previous = closing;
    static constexpr UINT_PTR id = 0x4d4d534e;
    static LRESULT CALLBACK Filter(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& s = *reinterpret_cast<WindowGuard*>(data);
        if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
            s.requested = true;
            for (unsigned i = 0; i < s.windows.size(); ++i) if (s.windows[i] == h) s.close[i] = true;
            return 0;
        }
        if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_HSCROLL || msg == WM_VSCROLL ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
        return DefSubclassProc(h, msg, wp, lp);
    }
    explicit WindowGuard(MMDApp& a) {
        windows = {a.MainWindow(), a.FloatingWindow() == a.MainWindow() ? nullptr : a.FloatingWindow()};
        closing = &requested;
        for (auto h : windows) if (h && !SetWindowSubclass(h, Filter, id, reinterpret_cast<DWORD_PTR>(this))) {
            Release(); throw std::bad_alloc();
        }
    }
    void Release() noexcept {
        closing = previous;
        for (unsigned i = 0; i < windows.size(); ++i) if (windows[i] && IsWindow(windows[i])) {
            RemoveWindowSubclass(windows[i], Filter, id);
            if (close[i]) PostMessageW(windows[i], WM_CLOSE, 0, 0);
        }
    }
    ~WindowGuard() { Release(); }
};
struct Field {
    std::string fixed;
    bool present = false;
    std::wstring wide, display;
    std::vector<unsigned char> allocation;
};
bool OwnedText(const wchar_t* p) noexcept {
    if (!p) return true;
    owned_text::Allocation a;
    if (!owned_text::Find(p, a) || a.bytes < sizeof(wchar_t) || a.bytes % sizeof(wchar_t)) return false;
    return wcsnlen_s(p, a.bytes / sizeof(wchar_t)) < a.bytes / sizeof(wchar_t);
}
// A replaced raw pointer must have exactly one model-owned field. Otherwise
// releasing its former owner would leave another native field dangling.
template<class Add> void ModelTexts(const MMDApp& app, Add&& add) noexcept {
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (const auto* raw = app.ModelSlot(slot)) {
        const auto& m = *mdl::Mdl(raw);
        for (auto* p : m.pmxTextBuffers) add(p);
        if (m.boneTable) for (std::uint32_t i = 0; i < m.boneCount; ++i) { add(m.boneTable[i].jpText); add(m.boneTable[i].enText); }
        if (m.morphs) for (std::uint32_t i = 0; i < m.morphCount; ++i) { add(m.morphs[i].jpText); add(m.morphs[i].enText); }
        if (m.rigidTable) for (std::uint32_t i = 0; i < m.rigidCount; ++i) { add(m.rigidTable[i].jpText); add(m.rigidTable[i].enText); }
        if (m.jointTable) for (std::uint32_t i = 0; i < m.jointCount; ++i) { add(m.jointTable[i].jpText); add(m.jointTable[i].enText); }
    }
}
Field Capture(const char* bytes, std::size_t capacity, const wchar_t* wide, std::wstring display) {
    Field field{std::string(bytes, capacity), wide != nullptr, wide ? wide : L"", std::move(display), {}};
    if (wide) {
        owned_text::Allocation a;
        if (!owned_text::Find(wide, a)) throw std::bad_alloc();
        const auto* begin = reinterpret_cast<const unsigned char*>(wide);
        field.allocation.assign(begin, begin + a.bytes);
    }
    return field;
}
bool Matches(const Field& f, const char* bytes, const wchar_t* wide) noexcept {
    if (std::memcmp(bytes, f.fixed.data(), f.fixed.size()) || (wide != nullptr) != f.present) return false;
    if (!wide) return true;
    owned_text::Allocation a;
    return owned_text::Find(wide, a) && a.bytes == f.allocation.size() &&
        !std::memcmp(wide, f.allocation.data(), f.allocation.size());
}
bool Equal(const Field& a, const Field& b) noexcept {
    return a.fixed == b.fixed && a.present == b.present && a.allocation == b.allocation;
}
bool Equal(const text_encoding::DisplayGroupTextState& a, const text_encoding::DisplayGroupTextState& b) noexcept {
    return a.present == b.present && a.names == b.names && a.legacy == b.legacy;
}
bool TextValid(const std::wstring& text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = std::uint16_t(text[i]);
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || std::uint16_t(text[i]) < 0xdc00 || std::uint16_t(text[i]) > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
std::wstring Lf(std::wstring value) { value.erase(std::remove(value.begin(), value.end(), L'\r'), value.end()); return value; }
std::wstring CrLf(const std::wstring& value) {
    std::wstring out; out.reserve(value.size());
    for (const auto c : value) { if (c == L'\n') out.push_back(L'\r'); out.push_back(c); }
    return out;
}
void Mirror(Field& field, const std::wstring& value) {
    std::string bytes;
    if (text_encoding::EncodeExact(value.c_str(), 932, bytes) && bytes.size() < field.fixed.size()) {
        std::fill(field.fixed.begin(), field.fixed.end(), '\0');
        std::memcpy(field.fixed.data(), bytes.data(), bytes.size());
    }
}
bool Stage(Field& field, const std::wstring& value) {
    if (!TextValid(value)) return false;
    if (value == field.display) return true;
    field.present = true; field.wide = value; field.display = value;
    const auto* first = reinterpret_cast<const unsigned char*>(value.c_str());
    field.allocation.assign(first, first + (value.size() + 1) * sizeof(wchar_t));
    Mirror(field, value); return true;
}
std::string Bounded(const std::string& bytes) { return bytes.substr(0, bytes.find('\0')); }
struct Snapshot {
    Field model, comment;
    std::vector<Field> bones, morphs, groups;
    std::vector<text_encoding::DisplayGroupTextState> sidecars;
    std::vector<std::array<char, 20>> boneRows, facialRows;
};
bool Equal(const Snapshot& a, const Snapshot& b) noexcept {
    if (!Equal(a.model, b.model) || !Equal(a.comment, b.comment) || a.boneRows != b.boneRows || a.facialRows != b.facialRows) return false;
    for (const auto pair : {std::pair{&a.bones, &b.bones}, {&a.morphs, &b.morphs}, {&a.groups, &b.groups}})
        for (std::size_t i = 0; i < pair.first->size(); ++i) if (!Equal((*pair.first)[i], (*pair.second)[i])) return false;
    for (std::size_t i = 0; i < a.sidecars.size(); ++i) if (!Equal(a.sidecars[i], b.sidecars[i])) return false;
    return true;
}
void Measure(const Snapshot& s, resource_budget::Collector& c) {
    const auto field = [&](const Field& f) { c.String(f.fixed); c.String(f.wide); c.String(f.display); c.Vector(f.allocation); };
    field(s.model); field(s.comment); c.Vector(s.bones); c.Vector(s.morphs); c.Vector(s.groups);
    for (const auto* fields : {&s.bones, &s.morphs, &s.groups}) for (const auto& f : *fields) field(f);
    c.Vector(s.sidecars); c.Vector(s.boneRows); c.Vector(s.facialRows);
    for (const auto& sidecar : s.sidecars) { for (const auto& v : sidecar.names) c.String(v); for (const auto& v : sidecar.legacy) c.String(v); }
}
struct FreeText { void operator()(wchar_t* p) const noexcept { owned_text::Free(p); } };
using NamesTextBuffer = std::unique_ptr<wchar_t, FreeText>;
}
struct NativeModelNamesPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app;
    SharedHistory* const history;
    const unsigned slot;
    unsigned char* const object;
    const MmsObjectRef initial;
    const std::uint64_t binding, scene, revision;
    std::shared_ptr<RetainedSceneObject> owner;
    mdl::BoneRecord* bones = nullptr;
    mdl::MorphRecord* morphs = nullptr;
    mdl::DisplayGroup* groups = nullptr;
    mdl::FrameGroup *boneRows = nullptr, *facialRows = nullptr;
    std::vector<std::uint32_t> boneTargets, facialTargets;
    std::vector<std::wstring> nativeGroups;
    std::vector<std::array<char, 50>> nativeGroupBytes;
    Snapshot before, after;
    std::uint64_t serial = 1, fixedBytes = 0, initialBookkeeping = 0;
    bool staged = false, applied = false, published = false;
    State(MMDApp& a, SharedHistory& h, unsigned s, std::uint64_t r)
        : app(a), history(&h), slot(s), object(a.ModelSlot(s)), initial(SceneObjectIdentity(object)),
          binding(BoundNativePoseGeneration(&a)), scene(a.SceneGeneration()), revision(r) {}
    bool Source(bool forward, bool first) const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || app.ModelSlot(slot) != object ||
            !owner->Present() || !SceneObjectPresent(object, initial.id) ||
            (first && (BoundNativePoseGeneration(&app) != binding || SceneObjectIdentity(object).generation != initial.generation))) return false;
        const auto& m = *mdl::Mdl(object); const auto& s = forward ? before : after;
        if (m.boneTable != bones || m.boneCount != s.bones.size() || m.morphs != morphs || m.morphCount != s.morphs.size() ||
            mdl::DisplayGroups(object) != groups || mdl::DisplayGroupCount(m) != s.groups.size() ||
            m.rbGroups != boneRows || m.rigidBodyCount != s.boneRows.size() || m.displayFrames != facialRows ||
            mdl::FacialFrameCount(m) != s.facialRows.size() ||
            !Matches(s.model, m.nameEn, m.pmxTextBuffers[1]) || !Matches(s.comment, m.commentEn, m.pmxTextBuffers[3])) return false;
        for (std::size_t i = 0; i < s.bones.size(); ++i) if (!Matches(s.bones[i], bones[i].nameEn, bones[i].enText)) return false;
        for (std::size_t i = 0; i < s.morphs.size(); ++i) if (!Matches(s.morphs[i], morphs[i].nameEn, morphs[i].enText)) return false;
        for (std::size_t i = 0; i < s.groups.size(); ++i)
            if (std::memcmp(groups[i].name, nativeGroupBytes[i].data(), 50) || !Matches(s.groups[i], groups[i].nameEn, nullptr) ||
                !text_encoding::MatchesDisplayGroupText(m, groups[i], s.sidecars[i])) return false;
        for (std::size_t i = 0; i < s.boneRows.size(); ++i)
            if (mdl::FrameTargetIndex(boneRows[i]) != boneTargets[i] || std::memcmp(boneRows[i].nameEn, s.boneRows[i].data(), 20)) return false;
        for (std::size_t i = 0; i < s.facialRows.size(); ++i)
            if (mdl::FrameTargetIndex(facialRows[i]) != facialTargets[i] || std::memcmp(facialRows[i].nameEn, s.facialRows[i].data(), 20)) return false;
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (!Source(true, true)) return false;
        std::uint64_t r = 0; return history->Revision(r) == MMS_OK && r == revision;
    }
    std::uint64_t MeasureRecord() const {
        resource_budget::Collector c; c.Array(this, 1); c.Add(this, sizeof(State) + 4 * sizeof(void*), 1);
        Measure(before, c); Measure(after, c); c.Vector(boneTargets); c.Vector(facialTargets); c.Vector(nativeGroups); c.Vector(nativeGroupBytes);
        for (const auto& n : nativeGroups) c.String(n);
        return c.Result().totalBytes;
    }
    struct Candidate final : PreparedHistoryChange {
        struct Write { char* target; const char* value; std::size_t size; };
        struct Unicode { wchar_t** target; wchar_t* expected; NamesTextBuffer handoff; };
        std::shared_ptr<State> state;
        const bool forward, first;
        const std::uint64_t serial, binding, generation;
        std::vector<Write> writes;
        std::vector<Unicode> unicode;
        struct Alias { const wchar_t* pointer; unsigned count; };
        mutable std::vector<Alias> aliases;
        text_encoding::PreparedDisplayGroupTextBatch sidecars;
        bool hasSidecars = false, consumed = false;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        Candidate(std::shared_ptr<State> s, bool f)
            : state(std::move(s)), forward(f), first(!state->published), serial(state->serial),
              binding(BoundNativePoseGeneration(&state->app)), generation(SceneObjectIdentity(state->object).generation) {}
        void Add(const Field& from, const Field& to, char* fixed, wchar_t** wide) {
            if (from.fixed != to.fixed) writes.push_back({fixed, to.fixed.data(), to.fixed.size()});
            if (wide && (from.present != to.present || from.allocation != to.allocation)) {
                NamesTextBuffer target;
                if (to.present) {
                    auto* p = static_cast<wchar_t*>(std::malloc(to.allocation.size()));
                    if (!p) throw std::bad_alloc();
                    target.reset(p); owned_text::Remember(p, to.allocation.size());
                    std::memcpy(p, to.allocation.data(), to.allocation.size());
                }
                unicode.push_back({wide, *wide, std::move(target)});
            }
        }
        void PrepareAliases() {
            for (const auto& u : unicode) if (u.expected) aliases.push_back({u.expected, 0});
            const auto less = [](const Alias& a, const Alias& b) { return std::less<const wchar_t*>{}(a.pointer, b.pointer); };
            std::sort(aliases.begin(), aliases.end(), less);
            aliases.erase(std::unique(aliases.begin(), aliases.end(), [](const Alias& a, const Alias& b) { return a.pointer == b.pointer; }), aliases.end());
        }
        bool Ownership() const noexcept {
            for (auto& a : aliases) a.count = 0;
            ModelTexts(state->app, [&](const wchar_t* p) {
                if (!p) return;
                const auto found = std::lower_bound(aliases.begin(), aliases.end(), p,
                    [](const Alias& a, const wchar_t* b) { return std::less<const wchar_t*>{}(a.pointer, b); });
                if (found != aliases.end() && found->pointer == p && found->count < 2) ++found->count;
            });
            for (const auto& a : aliases) if (a.count != 1) return false;
            return true;
        }
        bool IsCurrent() const noexcept override {
            if (consumed || state->serial != serial || state->applied == forward || !Ready(state->app, true) ||
                BoundNativePoseGeneration(&state->app) != binding || !state->Source(forward, first) ||
                SceneObjectIdentity(state->object).generation != generation || !state->owner->CoversBookkeeping(resources) ||
                (hasSidecars && !sidecars.IsCurrent())) return false;
            for (const auto& u : unicode) if (*u.target != u.expected) return false;
            return Ownership();
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (auto& u : unicode) { auto* old = *u.target; *u.target = u.handoff.release(); u.handoff.reset(old); }
            for (const auto& w : writes) std::memcpy(w.target, w.value, w.size);
            if (hasSidecars) sidecars.Publish();
            // Native model metadata needs a separate model-file save. Keep
            // the existing sticky close reminder, including after traversal;
            // undo must not clear a reminder set by another model edit.
            state->app.EnhancedModelDirty() = 1;
            state->applied = forward; state->published = true; ++state->serial; ++uiRevision; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!staged || applied == forward || !Source(forward, !published) || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, true)) return MMS_E_BUSY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            if (auto r = owner->MeasureBookkeeping(next->resources)) return r;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            const auto& source = forward ? before : after; const auto& target = forward ? after : before;
            auto& m = *mdl::Mdl(object);
            next->Add(source.model, target.model, m.nameEn, &m.pmxTextBuffers[1]);
            next->Add(source.comment, target.comment, m.commentEn, &m.pmxTextBuffers[3]);
            for (std::size_t i = 0; i < source.bones.size(); ++i) next->Add(source.bones[i], target.bones[i], bones[i].nameEn, &bones[i].enText);
            for (std::size_t i = 0; i < source.morphs.size(); ++i) next->Add(source.morphs[i], target.morphs[i], morphs[i].nameEn, &morphs[i].enText);
            std::vector<std::pair<const mdl::DisplayGroup*, text_encoding::DisplayGroupTextState>> changed;
            for (std::size_t i = 0; i < source.groups.size(); ++i) {
                next->Add(source.groups[i], target.groups[i], groups[i].nameEn, nullptr);
                if (!Equal(source.sidecars[i], target.sidecars[i])) changed.emplace_back(&groups[i], target.sidecars[i]);
            }
            for (std::size_t i = 0; i < source.boneRows.size(); ++i) if (source.boneRows[i] != target.boneRows[i])
                next->writes.push_back({boneRows[i].nameEn, target.boneRows[i].data(), 20});
            for (std::size_t i = 0; i < source.facialRows.size(); ++i) if (source.facialRows[i] != target.facialRows[i])
                next->writes.push_back({facialRows[i].nameEn, target.facialRows[i].data(), 20});
            if (!changed.empty()) {
                if (!next->sidecars.Prepare(m, changed)) return MMS_E_MEMORY;
                next->hasSidecars = true;
            }
            next->bytes = resource_budget::Collector::Sum(fixedBytes, next->resources.Result().totalBytes);
            next->bytes = resource_budget::Collector::Sum(next->bytes, next->sidecars.RetainedBytes());
            for (const auto& u : next->unicode) if (u.handoff) {
                owned_text::Allocation allocation;
                if (!owned_text::Find(u.handoff.get(), allocation)) return MMS_E_NOT_READY;
                next->bytes = resource_budget::Collector::Sum(next->bytes,
                    resource_budget::Collector::Sum(allocation.bytes, allocation.registryBytes));
            }
            next->PrepareAliases();
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return fixedBytes > UINT64_MAX - initialBookkeeping ? UINT64_MAX : fixedBytes + initialBookkeeping;
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult NativeModelNamesPlan::Begin(MMDApp& a, unsigned slot) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&a); if (!history) return MMS_E_NOT_READY;
        if (slot >= kModelSlotCount || !a.ModelSlot(slot)) return MMS_E_ARGUMENT;
        if (!Ready(a, false)) return MMS_E_BUSY;
        if (auto r = history->CheckNativeEdit()) return r;
        std::uint64_t revision = 0; if (auto r = history->Revision(revision)) return r;
        auto s = std::make_shared<State>(a, *history, slot, revision);
        s->owner = RetainSceneObject(a, MMS_OBJECT_MODEL, slot);
        auto& m = *mdl::Mdl(s->object); s->bones = m.boneTable; s->morphs = m.morphs;
        s->groups = mdl::DisplayGroups(s->object); s->boneRows = static_cast<mdl::FrameGroup*>(m.rbGroups); s->facialRows = m.displayFrames;
        if ((m.boneCount && !s->bones) || (m.morphCount && !s->morphs) || (mdl::DisplayGroupCount(m) && !s->groups) ||
            (m.rigidBodyCount && !s->boneRows) || (mdl::FacialFrameCount(m) && !s->facialRows)) return MMS_E_STALE;
        if (!OwnedText(m.pmxTextBuffers[1]) || !OwnedText(m.pmxTextBuffers[3])) return MMS_E_NOT_READY;
        for (std::uint32_t i = 0; i < m.boneCount; ++i) if (!OwnedText(s->bones[i].enText)) return MMS_E_NOT_READY;
        for (std::uint32_t i = 0; i < m.morphCount; ++i) if (!OwnedText(s->morphs[i].enText)) return MMS_E_NOT_READY;
        s->before.model = Capture(m.nameEn, sizeof m.nameEn, m.pmxTextBuffers[1], text_encoding::ModelName(m, true));
        std::wstring comment;
        if (m.pmxTextBuffers[3] && *m.pmxTextBuffers[3]) comment = m.pmxTextBuffers[3];
        else { const std::string bounded(m.commentEn, strnlen_s(m.commentEn, sizeof m.commentEn)); comment = text_encoding::Display(bounded.c_str()); }
        s->before.comment = Capture(m.commentEn, sizeof m.commentEn, m.pmxTextBuffers[3], Lf(std::move(comment)));
        for (std::uint32_t i = 0; i < m.boneCount; ++i)
            s->before.bones.push_back(Capture(s->bones[i].nameEn, sizeof s->bones[i].nameEn, s->bones[i].enText, text_encoding::BoneName(s->bones[i], true)));
        for (std::uint32_t i = 0; i < m.morphCount; ++i)
            s->before.morphs.push_back(Capture(s->morphs[i].nameEn, sizeof s->morphs[i].nameEn, s->morphs[i].enText, text_encoding::MorphName(s->morphs[i], true)));
        for (std::uint32_t i = 0; i < mdl::DisplayGroupCount(m); ++i) {
            s->before.groups.push_back(Capture(s->groups[i].nameEn, sizeof s->groups[i].nameEn, nullptr, text_encoding::DisplayGroupName(s->groups[i], true)));
            s->nativeGroups.push_back(text_encoding::DisplayGroupName(s->groups[i], false));
            std::array<char, 50> native; std::memcpy(native.data(), s->groups[i].name, native.size());
            s->nativeGroupBytes.push_back(native);
            text_encoding::DisplayGroupTextState cache;
            if (!text_encoding::CaptureDisplayGroupText(m, s->groups[i], cache)) return MMS_E_STALE;
            s->before.sidecars.push_back(std::move(cache));
        }
        for (std::uint32_t i = 0; i < m.rigidBodyCount; ++i) {
            std::array<char, 20> row; std::memcpy(row.data(), s->boneRows[i].nameEn, 20);
            s->before.boneRows.push_back(row); s->boneTargets.push_back(mdl::FrameTargetIndex(s->boneRows[i]));
        }
        for (std::uint32_t i = 0; i < mdl::FacialFrameCount(m); ++i) {
            std::array<char, 20> row; std::memcpy(row.data(), s->facialRows[i].nameEn, 20);
            s->before.facialRows.push_back(row); s->facialTargets.push_back(mdl::FrameTargetIndex(s->facialRows[i]));
        }
        s->after = s->before; s->fixedBytes = s->MeasureRecord();
        resource_budget::Collector c; if (auto r = s->owner->MeasureBookkeeping(c)) return r;
        if (!c.Result().Complete()) return MMS_E_NOT_READY;
        s->initialBookkeeping = c.Result().totalBytes;
        if (!s->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(s); return MMS_OK;
    });
}
MmsResult NativeModelNamesPlan::Read(ModelNamesDraft& out) const noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if (!state_->InitialCurrent()) return MMS_E_STALE;
        ModelNamesDraft value; const auto& s = state_->after;
        value.model = s.model.display; value.comment = s.comment.display;
        for (const auto& n : s.bones) value.bones.push_back(n.display);
        for (const auto& n : s.morphs) value.morphs.push_back(n.display);
        for (const auto& n : s.groups) value.groups.push_back(n.display);
        out = std::move(value); return MMS_OK;
    });
}
MmsResult NativeModelNamesPlan::Set(const ModelNamesDraft& value) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& s = *state_; if (!s.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(s.app, false)) return MMS_E_BUSY;
        if (value.bones.size() != s.before.bones.size() || value.morphs.size() != s.before.morphs.size() || value.groups.size() != s.before.groups.size()) return MMS_E_ARGUMENT;
        auto next = s.before;
        if (!Stage(next.model, value.model) || !Stage(next.comment, Lf(value.comment))) return MMS_E_ARGUMENT;
        for (std::size_t i = 0; i < next.bones.size(); ++i) if (!Stage(next.bones[i], value.bones[i])) return MMS_E_ARGUMENT;
        const bool facial = !s.facialTargets.empty();
        for (std::size_t i = 0; i < next.morphs.size(); ++i) {
            if (!i || !facial) { if (value.morphs[i] != next.morphs[i].display) return MMS_E_ARGUMENT; }
            else if (!Stage(next.morphs[i], value.morphs[i])) return MMS_E_ARGUMENT;
        }
        const std::size_t firstGroup = facial ? 3 : 2;
        for (std::size_t i = 0; i < next.groups.size(); ++i) {
            if (!TextValid(value.groups[i])) return MMS_E_ARGUMENT;
            if (i < firstGroup) { if (value.groups[i] != next.groups[i].display) return MMS_E_ARGUMENT; continue; }
            if (value.groups[i] == next.groups[i].display) continue;
            Mirror(next.groups[i], value.groups[i]); next.groups[i].display = value.groups[i];
            auto& cache = next.sidecars[i]; cache.present = true; cache.names = {s.nativeGroups[i], value.groups[i]};
            cache.legacy = {std::string(s.groups[i].name, strnlen_s(s.groups[i].name, sizeof s.groups[i].name)), Bounded(next.groups[i].fixed)};
        }
        if (!Equal(next, s.before)) {
            for (std::size_t i = 0; i < next.boneRows.size(); ++i) if (s.boneTargets[i] < next.bones.size())
                std::memcpy(next.boneRows[i].data(), next.bones[s.boneTargets[i]].fixed.data(), 20);
            for (std::size_t i = 0; i < next.facialRows.size(); ++i) if (s.facialTargets[i] < next.morphs.size())
                std::memcpy(next.facialRows[i].data(), next.morphs[s.facialTargets[i]].fixed.data(), 20);
        }
        if (!s.InitialCurrent()) return MMS_E_STALE;
        // Compute the new immutable payload before replacing an earlier draft.
        Snapshot old = std::move(s.after); s.after = std::move(next);
        try { s.fixedBytes = s.MeasureRecord(); }
        catch (...) { s.after = std::move(old); throw; }
        s.staged = true; return MMS_OK;
    });
}
MmsResult NativeModelNamesPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->staged) return MMS_E_NOT_READY;
    if (!state_->InitialCurrent()) return MMS_E_STALE;
    if (!Equal(state_->before, state_->after)) out = std::move(state_); else state_.reset();
    return MMS_OK;
}
std::uint64_t NativeModelNamesUiRevision() noexcept { return uiRevision; }
namespace {
struct Dialog {
    MMDApp& app; ModelNamesDraft draft; unsigned firstGroup = 0;
    std::array<int, 3> cursors{};
    MmsResult error = MMS_OK;
    HWND composing = nullptr;
    bool updating = false;
    static constexpr std::array<int, 3> combos{669, 673, 677}, edits{672, 676, 680};
    std::vector<std::wstring>& Names(unsigned family) { return family == 0 ? draft.bones : family == 1 ? draft.morphs : draft.groups; }
    unsigned First(unsigned family) const noexcept { return family == 0 ? 0 : family == 1 ? 1 : firstGroup; }
    static std::wstring Text(HWND h) {
        const auto length = GetWindowTextLengthW(h); if (length < 0 || length == INT_MAX) throw std::bad_alloc();
        std::wstring value(std::size_t(length) + 1, L'\0');
        const int copied = GetWindowTextW(h, value.data(), int(value.size())); value.resize(copied); return value;
    }
    static MmsResult WriteText(HWND h, const std::wstring& value) {
        // The setter wrappers need not propagate a control's WM_SETTEXT result.
        // Keep both its real acknowledgement and the exact Unicode contents.
        if (!h || !IsWindowUnicode(h) || !SendMessageW(h, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())))
            return MMS_E_FAILED;
        return Text(h) == value ? MMS_OK : MMS_E_FAILED;
    }
    void Save(HWND h) {
        draft.model = Text(GetDlgItem(h, 667)); draft.comment = Lf(Text(GetDlgItem(h, 668)));
        for (unsigned f = 0; f < 3; ++f) if (cursors[f] >= 0 && std::size_t(cursors[f]) < Names(f).size())
            Names(f)[cursors[f]] = Text(GetDlgItem(h, edits[f]));
    }
    MmsResult Fill(HWND h, unsigned f) {
        const bool old = updating; updating = true;
        auto combo = GetDlgItem(h, combos[f]); auto edit = GetDlgItem(h, edits[f]); auto& names = Names(f);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (std::size_t i = First(f); i < names.size(); ++i)
            if (SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i].c_str())) < 0) throw std::bad_alloc();
        const bool enabled = cursors[f] >= 0 && std::size_t(cursors[f]) < names.size();
        EnableWindow(combo, enabled); EnableWindow(edit, enabled);
        for (int id : {combos[f] + 1, combos[f] + 2}) EnableWindow(GetDlgItem(h, id), enabled);
        SendMessageW(combo, CB_SETCURSEL, enabled ? cursors[f] - First(f) : -1, 0);
        const auto result = enabled ? WriteText(edit, names[cursors[f]]) : WriteText(edit, {});
        updating = old; return result;
    }
    static LRESULT CALLBACK Edit(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& s = *reinterpret_cast<Dialog*>(data);
        if (msg == WM_IME_STARTCOMPOSITION) s.composing = h;
        if (msg == WM_IME_ENDCOMPOSITION && s.composing == h) s.composing = nullptr;
        if (msg == WM_GETDLGCODE && (wp == VK_RETURN || s.composing == h))
            return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTALLKEYS;
        if (msg == WM_KEYDOWN && wp == VK_RETURN && GetDlgCtrlID(h) != 668 && s.composing != h) {
            const auto r = Boundary([&]() -> MmsResult { s.Save(GetParent(h)); return MMS_OK; });
            if (r) { s.error = r; EndDialog(GetParent(h), IDCANCEL); } return 0;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, Edit, 0x4d4d534e);
        return DefSubclassProc(h, msg, wp, lp);
    }
    static INT_PTR CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* s = reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
        if (msg == WM_INITDIALOG) { s = reinterpret_cast<Dialog*>(lp); SetWindowLongPtrW(h, DWLP_USER, lp); }
        if (!s) return FALSE;
        bool handled = false;
        const auto result = Boundary([&]() -> MmsResult {
            if (msg == WM_INITDIALOG) {
                handled = true; s->updating = true;
                if (!IsWindowUnicode(h)) return MMS_E_FAILED;
                if (const auto result = WriteText(GetDlgItem(h, 667), s->draft.model)) return result;
                if (const auto result = WriteText(GetDlgItem(h, 668), CrLf(s->draft.comment))) return result;
                for (int id : {667, 668, 672, 676, 680}) {
                    const auto edit = GetDlgItem(h, id); if (!edit || !IsWindowUnicode(edit) || !SetWindowSubclass(edit, Edit, 0x4d4d534e, reinterpret_cast<DWORD_PTR>(s))) return MMS_E_FAILED;
                    SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
                }
                for (const int id : combos) if (!IsWindowUnicode(GetDlgItem(h, id))) return MMS_E_FAILED;
                for (unsigned f = 0; f < 3; ++f) {
                    s->cursors[f] = (f != 1 || s->firstGroup == 3) && s->Names(f).size() > s->First(f) ? int(s->First(f)) : -1;
                    if (const auto result = s->Fill(h, f)) return result;
                }
                RECT ok{65, 207, 107, 221}, cancel{119, 207, 169, 221}; MapDialogRect(h, &ok); MapDialogRect(h, &cancel);
                MoveWindow(GetDlgItem(h, IDOK), ok.left, ok.top, ok.right - ok.left, ok.bottom - ok.top, FALSE);
                auto button = CreateWindowExW(0, L"BUTTON", s->app.EnglishUI() == 1 ? L"Cancel" : s->app.EnglishUI() == 2 ? L"取消" : L"キャンセル",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, cancel.left, cancel.top, cancel.right - cancel.left, cancel.bottom - cancel.top,
                    h, reinterpret_cast<HMENU>(INT_PTR(IDCANCEL)), GetModuleHandleW(nullptr), nullptr);
                if (!button) return MMS_E_FAILED;
                SendMessageW(button, WM_SETFONT, SendMessageW(GetDlgItem(h, IDOK), WM_GETFONT, 0, 0), TRUE);
                s->updating = false; SetFocus(GetDlgItem(h, 667)); return MMS_OK;
            }
            if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) { handled = true; EndDialog(h, IDCANCEL); return MMS_OK; }
            if (msg != WM_COMMAND || s->updating) return MMS_OK;
            const int id = LOWORD(wp);
            if (id == IDOK) {
                handled = true; if (s->composing) return MMS_OK;
                s->Save(h); EndDialog(h, IDOK); return MMS_OK;
            }
            for (unsigned f = 0; f < 3; ++f) {
                const int base = combos[f];
                if ((id == base && HIWORD(wp) == CBN_SELCHANGE) || id == base + 1 || id == base + 2) {
                    handled = true; if (s->composing) return MMS_OK;
                    if (f == 1 && s->firstGroup != 3) return MMS_OK;
                    int row = int(SendDlgItemMessageW(h, base, CB_GETCURSEL, 0, 0));
                    s->Save(h); const auto count = int(s->Names(f).size()) - int(s->First(f)); if (count <= 0) return MMS_OK;
                    if (id == base + 1) row = (s->cursors[f] - int(s->First(f)) + 1) % count;
                    if (id == base + 2) row = (s->cursors[f] - int(s->First(f)) + count - 1) % count;
                    if (row < 0 || row >= count) return MMS_E_ARGUMENT;
                    s->cursors[f] = row + int(s->First(f)); return s->Fill(h, f);
                }
            }
            return MMS_OK;
        });
        if (result) { s->error = result; EndDialog(h, IDCANCEL); return TRUE; }
        return handled ? TRUE : FALSE;
    }
};
}
bool TryEditBoundModelNames(MMDApp* app, void* ownerWindow) noexcept {
    if (!app) return false; auto* history = BoundNativePoseHistory(app); if (!history) return false;
    static thread_local bool active = false; if (active) return true;
    const auto result = Boundary([&]() -> MmsResult {
        if (app->CameraMode() || g_Block != app) return MMS_E_NOT_READY;
        struct Active { bool& b; Active(bool& v) : b(v) { b = true; } ~Active() { b = false; } } guard(active);
        NativeModelNamesPlan plan; if (auto r = plan.Begin(*app, app->SelectedModelSlot())) return r;
        Dialog dialog{*app}; if (auto r = plan.Read(dialog.draft)) return r;
        dialog.firstGroup = mdl::FacialFrameCount(*mdl::Mdl(app->SelectedModel())) ? 3 : 2;
        // No facial rows means the legacy editor does not expose morph names.
        if (dialog.firstGroup == 2) dialog.cursors[1] = -1;
        WindowGuard windows(*app);
        INT_PTR answer = -1;
        { bake::ScopedUiInputGate gate; answer = UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()),
            MAKEINTRESOURCEW(app->EnglishUI() ? 666 : 665), static_cast<HWND>(ownerWindow), Dialog::Proc, reinterpret_cast<LPARAM>(&dialog)); }
        if (dialog.error) return dialog.error;
        if (answer == -1) return MMS_E_FAILED;
        if (answer != IDOK || windows.requested) return MMS_OK;
        if (auto r = plan.Set(dialog.draft)) return r;
        std::shared_ptr<HistoryChange> change; if (auto r = plan.Take(change)) return r;
        if (!change) return MMS_OK;
        {
            struct PublishScope { const MMDApp* before; PublishScope(MMDApp* a) : before(publication) { publication = a; } ~PublishScope() { publication = before; } } authorized(app);
            bake::ScopedUiInputGate gate;
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative("Model English names", std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
        }
        PublishBoundPoseEdit(*app, *history, false);
        RefreshBoundObjectUi(*app, *history); return MMS_OK;
    });
    if (result) runtime_log::Write("PLUGIN_ABI model English names refused: %d", result);
    return true;
}
}
#endif
