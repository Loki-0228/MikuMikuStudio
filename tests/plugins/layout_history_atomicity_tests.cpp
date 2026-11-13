#include "../../src/plugins/ui_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

namespace {
thread_local bool trackAllocations = false, injected = false;
thread_local std::size_t allocationIndex = 0, failAt = std::numeric_limits<std::size_t>::max();
}
void* operator new(std::size_t size) {
    if (trackAllocations && allocationIndex++ == failAt) { injected = true; throw std::bad_alloc(); }
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (false)
MmsString S(const char* value) { return {value, std::strlen(value)}; }
constexpr const char* elementId = "example.layout_atomicity.button";
struct Fixture {
    unsigned notifications = 0;
    std::function<void()> observer;
    UiRegistry registry{[this] {
        ++notifications;
        // A one-shot observer makes nested mutations finite and intentional.
        if (observer) { auto callback = std::move(observer); observer = {}; callback(); }
    }};
    UiClient client{&registry, "example.layout_atomicity"};
    MmsUiApiV1 api = registry.Api(client);
    MmsUiLayoutApiV1_1 history = registry.LayoutHistoryApi(client);
    std::string hidden, visible;
    Fixture() {
        MmsCommandDescV1 command{};
        command.info.h = Header<MmsCommandDescV1>();
        command.info.id = S("example.layout_atomicity.run"); command.info.ownerPlugin = S("example.layout_atomicity");
        command.getState = [](void*, MmsObjectRef, MmsCommandStateV1* state) -> MmsResult {
            state->enabled = state->visible = 1; return MMS_OK;
        };
        command.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
        MmsHandle handle = 0;
        CHECK(api.registerCommand(api.context, &command, &handle) == MMS_OK);
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S(elementId);
        element.ownerPlugin = command.info.ownerPlugin; element.commandId = command.info.id;
        element.parentId = S("mms.toolbar.main.extensions"); element.kind = MMS_UI_BUTTON;
        element.capabilities = MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_LAYOUT;
        CHECK(api.registerElement(api.context, &element, &handle) == MMS_OK);
        CHECK(Commit(false) == MMS_OK); hidden = registry.ExportPreferences();
        CHECK(Commit(true) == MMS_OK); visible = registry.ExportPreferences();
    }
    MmsHandle Stage(bool visible) {
        MmsHandle transaction = 0;
        CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_VISIBLE;
        op.elementId = S(elementId); op.expectedGeneration = registry.elements.at(elementId).desc.generation;
        op.value.visible = visible ? 1 : 0;
        CHECK(api.applyLayout(api.context, transaction, &op) == MMS_OK);
        return transaction;
    }
    MmsResult Commit(bool visible) { const auto tx = Stage(visible); return api.commitLayout(api.context, tx); }
    MmsUiLayoutHistoryInfoV1 Info() {
        MmsUiLayoutHistoryInfoV1 info{}; info.h = Header<MmsUiLayoutHistoryInfoV1>();
        CHECK(history.queryHistory(history.context, &info) == MMS_OK); return info;
    }
};
struct Snapshot {
    std::string json;
    uint64_t revision, preferenceRevision, generation;
    unsigned undoDepth, redoDepth, notifications;
    bool visible;
};
Snapshot Capture(Fixture& f) {
    const auto info = f.Info(); const auto& element = f.registry.elements.at(elementId);
    return {f.registry.ExportPreferences(), f.registry.Revision(), f.registry.PreferencesRevision(),
        element.desc.generation, info.undoDepth, info.redoDepth, f.notifications, element.visible};
}
void Same(const Snapshot& a, const Snapshot& b) {
    CHECK(a.json == b.json && a.visible == b.visible && a.generation == b.generation);
    CHECK(a.revision == b.revision && a.preferenceRevision == b.preferenceRevision);
    CHECK(a.undoDepth == b.undoDepth && a.redoDepth == b.redoDepth && a.notifications == b.notifications);
}
enum class Action { Commit, Undo, Redo, Import };
std::size_t Exercise(Action action, std::size_t index, bool requireInjection) {
    Fixture f;
    if (action == Action::Redo || action == Action::Commit) CHECK(f.history.undo(f.history.context) == MMS_OK);
    const auto stable = Capture(f);
    const bool expectedVisible = action == Action::Redo || action == Action::Commit;
    const auto transaction = action == Action::Commit ? f.Stage(true) : MmsHandle(0);
    allocationIndex = 0; failAt = index; injected = false; trackAllocations = true;
    MmsResult result = MMS_E_FAILED;
    switch (action) {
    case Action::Commit: result = f.api.commitLayout(f.api.context, transaction); break;
    case Action::Undo: result = f.history.undo(f.history.context); break;
    case Action::Redo: result = f.history.redo(f.history.context); break;
    case Action::Import: result = f.registry.ImportPreferences(f.hidden); break;
    }
    trackAllocations = false;
    const auto count = allocationIndex;
    if (requireInjection) CHECK(injected);
    const auto after = Capture(f);
    if (result == MMS_E_MEMORY) {
        Same(stable, after);
        // A failed transfer must leave both the source and inverse usable.
        if (action == Action::Redo) CHECK(f.history.redo(f.history.context) == MMS_OK);
        else if (action == Action::Undo) CHECK(f.history.undo(f.history.context) == MMS_OK);
        else if (action == Action::Commit) CHECK(f.Commit(true) == MMS_OK);
        else CHECK(f.registry.ImportPreferences(f.hidden) == MMS_OK);
    } else {
        CHECK(result == MMS_OK);
        CHECK(after.json == (expectedVisible ? f.visible : f.hidden));
        CHECK(after.visible == expectedVisible);
        CHECK(after.revision == stable.revision + 1 && after.preferenceRevision == stable.preferenceRevision + 1);
        CHECK(after.generation != stable.generation && after.notifications == stable.notifications + 1);
        const unsigned undo = action == Action::Commit ? stable.undoDepth + 1 :
            action == Action::Undo ? stable.undoDepth - 1 : action == Action::Redo ? stable.undoDepth + 1 : stable.undoDepth;
        const unsigned redo = action == Action::Commit ? 0 : action == Action::Undo ? stable.redoDepth + 1 : action == Action::Redo ? stable.redoDepth - 1 : stable.redoDepth;
        CHECK(after.undoDepth == undo && after.redoDepth == redo);
    }
    return count;
}
void AllocationChecks() {
    for (const auto action : {Action::Commit, Action::Undo, Action::Redo, Action::Import}) {
        const auto count = Exercise(action, std::numeric_limits<std::size_t>::max(), false);
        CHECK(count > 0 && count < 4096);
        for (std::size_t i = 0; i < count; ++i) Exercise(action, i, true);
        std::printf("layout history allocation sweep action=%d points=%zu\n", int(action), count);
    }
}
void ReentryChecks() {
    Fixture f;
    f.observer = [&] {
        const auto info = f.Info(); CHECK(info.undoDepth == 1 && info.redoDepth == 1);
        CHECK(!f.registry.elements.at(elementId).visible);
        CHECK(f.history.redo(f.history.context) == MMS_OK);
    };
    CHECK(f.history.undo(f.history.context) == MMS_OK);
    CHECK(f.registry.elements.at(elementId).visible && f.Info().undoDepth == 2 && f.Info().redoDepth == 0);
    CHECK(f.history.undo(f.history.context) == MMS_OK);
    f.observer = [&] {
        const auto info = f.Info(); CHECK(info.undoDepth == 2 && info.redoDepth == 0);
        CHECK(f.registry.elements.at(elementId).visible);
        CHECK(f.Commit(false) == MMS_OK);
    };
    CHECK(f.history.redo(f.history.context) == MMS_OK);
    CHECK(!f.registry.elements.at(elementId).visible && f.Info().undoDepth == 3 && f.Info().redoDepth == 0);
    f.observer = [&] {
        const auto info = f.Info(); CHECK(info.undoDepth == 4 && info.redoDepth == 0);
        CHECK(f.history.undo(f.history.context) == MMS_OK);
    };
    CHECK(f.Commit(true) == MMS_OK);
    CHECK(!f.registry.elements.at(elementId).visible && f.Info().undoDepth == 3 && f.Info().redoDepth == 1);
    // An equal-value commit must not consume the available redo record.
    CHECK(f.Commit(false) == MMS_OK);
    CHECK(f.Info().undoDepth == 3 && f.Info().redoDepth == 1);
    CHECK(f.history.redo(f.history.context) == MMS_OK && f.registry.elements.at(elementId).visible);
}
void CapacityChecks() {
    Fixture f;
    for (unsigned i = 0; i < 40; ++i) CHECK(f.Commit(i % 2 != 0) == MMS_OK);
    CHECK(f.Info().undoDepth == 32 && f.Info().redoDepth == 0);
    for (unsigned i = 0; i < 32; ++i) CHECK(f.history.undo(f.history.context) == MMS_OK);
    CHECK(f.Info().undoDepth == 0 && f.Info().redoDepth == 32);
    CHECK(f.history.undo(f.history.context) == MMS_E_NOT_READY);
    for (unsigned i = 0; i < 32; ++i) CHECK(f.history.redo(f.history.context) == MMS_OK);
    CHECK(f.Info().undoDepth == 32 && f.Info().redoDepth == 0);
    CHECK(f.history.redo(f.history.context) == MMS_E_NOT_READY);
}
}
int main() {
    try { AllocationChecks(); ReentryChecks(); CapacityChecks(); }
    catch (...) { trackAllocations = false; CHECK(false); }
    std::printf("layout_history_atomicity_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
