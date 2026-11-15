// MAINTENANCE: Keep the UI-08 host transaction regression until an equivalent port covers these cases.
#include "../src/plugins/workspace_layout_preview.hpp"

#include <cstdio>

using namespace mikumikustudio::plugins::workspace_layout;

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return false; } } while (false)

namespace {
const Source controller{100, {200, 1}}, wrongOwner{101, {200, 1}}, newSurface{100, {200, 2}};
constexpr Region workspace = 1, otherRegion = 2;
const Ref nativePane{1, 1}, pluginPane{2, 1}, timeline{3, 1}, otherPane{4, 1};
const Ref nativeOrigin{11, 1}, pluginOrigin{12, 1}, timelineSlot{13, 1}, otherOrigin{14, 1};
const Ref spare1{15, 1}, spare2{16, 1}, hiddenSlot{17, 1}, foreignSlot{18, 1}, noOffer{19, 1};
const Move nativeYield{nativePane, spare1}, pluginYield{pluginPane, spare2};

HostLayout Layout() {
    return {{11, 23},
        {{nativePane, nativeOrigin, true}, {pluginPane, pluginOrigin, true},
         {timeline, timelineSlot, false}, {otherPane, otherOrigin, true}},
        {{nativeOrigin, workspace}, {pluginOrigin, workspace}, {timelineSlot, workspace, true},
         {otherOrigin, otherRegion}, {spare1, workspace, true}, {spare2, workspace, true},
         {hiddenSlot, workspace}, {foreignSlot, otherRegion, true}, {noOffer, workspace, true}},
        controller,
        {nativeYield, pluginYield, {pluginPane, spare1}, {nativePane, timelineSlot},
         {timeline, spare2}, {nativePane, foreignSlot}, {nativePane, hiddenSlot},
         {otherPane, foreignSlot}}};
}

bool FullReplacementAndCancel() {
    Preview core;
    const auto live = Layout();
    SessionState state;
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane, pluginPane}, state) == Result::Ok);
    CHECK(state.token && state.revision == 1);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {nativeYield, pluginYield}) == Result::Ok);
    CHECK(core.EffectiveSlot(live, nativePane) == spare1);
    CHECK(core.EffectiveSlot(live, pluginPane) == spare2);
    CHECK(core.EffectiveSlot(live, timeline) == timelineSlot);
    CHECK(core.EffectiveSlot(live, otherPane) == otherOrigin);
    CHECK(live.panes[0].slot == nativeOrigin && live.panes[1].slot == pluginOrigin);
    CHECK(live.basis == Layout().basis);

    CHECK(core.Query(controller, state.token, live.basis, state) == Result::Ok && state.revision == 2);
    // The first preview's destination is vacant in the committed layout; it
    // can be reused in the replacement while the omitted pane returns home.
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {{pluginPane, spare1}}) == Result::Ok);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.EffectiveSlot(live, pluginPane) == spare1);
    CHECK(core.Query(controller, state.token, live.basis, state) == Result::Ok && state.revision == 3);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {}) == Result::Ok);
    CHECK(core.EffectiveSlot(live, pluginPane) == pluginOrigin);
    CHECK(core.Query(controller, state.token, live.basis, state) == Result::Ok && state.revision == 4);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {nativeYield}) == Result::Ok);
    CHECK(core.Cancel(controller, state.token) == Result::Ok);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.Query(controller, state.token, live.basis, state) == Result::NotFound);
    return true;
}

bool AtomicAndScoped() {
    Preview core;
    auto live = Layout();
    SessionState state{777, 888};
    CHECK(core.Begin(controller, workspace, live.basis, live, {}, state) == Result::Invalid);
    CHECK(core.Begin(controller, workspace, live.basis, live, std::vector<Ref>(9, nativePane), state) == Result::Invalid);
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane, nativePane}, state) == Result::Invalid);
    CHECK(core.Begin(controller, workspace, live.basis, live, {timeline}, state) == Result::NotOffered);
    CHECK(core.Begin(controller, workspace, live.basis, live, {otherPane}, state) == Result::NotOffered);
    CHECK(core.Begin(wrongOwner, workspace, live.basis, live, {nativePane}, state) == Result::NotOffered);
    CHECK(core.Begin(controller, workspace, {10, 23}, live, {nativePane}, state) == Result::Stale);
    auto occupied = live;
    occupied.offers = {{nativePane, timelineSlot}};
    CHECK(core.Begin(controller, workspace, occupied.basis, occupied, {nativePane}, state) == Result::NotOffered);
    CHECK(state.token == 777 && state.revision == 888);
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane, pluginPane}, state) == Result::Ok);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {pluginYield}) == Result::Ok);
    CHECK(core.Query(controller, state.token, live.basis, state) == Result::Ok);

    const auto submit = [&](const std::vector<Move>& moves) {
        return core.Replace(controller, state.token, state.revision, live.basis, live, moves);
    };
    CHECK(submit({nativeYield, {timeline, spare2}}) == Result::NotOffered);
    CHECK(submit({nativeYield, {otherPane, foreignSlot}}) == Result::NotOffered);
    CHECK(submit({{nativePane, foreignSlot}, pluginYield}) == Result::NotOffered);
    CHECK(submit({{nativePane, hiddenSlot}, pluginYield}) == Result::NotOffered);
    CHECK(submit({{nativePane, noOffer}, pluginYield}) == Result::NotOffered);
    CHECK(submit({{nativePane, timelineSlot}, pluginYield}) == Result::Conflict);
    CHECK(submit({nativeYield, nativeYield}) == Result::Invalid);
    CHECK(submit({nativeYield, {pluginPane, spare1}}) == Result::Conflict);
    CHECK(submit({nativeYield, {{40, 1}, spare2}}) == Result::NotFound);
    CHECK(submit({nativeYield, {pluginPane, {50, 1}}}) == Result::NotFound);
    CHECK(submit({nativeYield, {{2, 2}, spare2}}) == Result::Stale);
    CHECK(submit({nativeYield, {pluginPane, {16, 2}}}) == Result::Stale);
    CHECK(submit({nativeYield, {pluginPane, {}}}) == Result::Invalid);
    live.offerSource = wrongOwner;
    CHECK(submit({nativeYield}) == Result::NotOffered);
    CHECK(core.EffectiveSlot(live, timeline) == timelineSlot);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.EffectiveSlot(live, pluginPane) == spare2);
    SessionState after;
    CHECK(core.Query(controller, state.token, live.basis, after) == Result::Ok);
    CHECK(after.revision == state.revision);
    return true;
}

bool OneOwnerAndStaleRequests() {
    Preview core;
    const auto live = Layout();
    SessionState owner, sentinel{777, 888};
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, owner) == Result::Ok);
    auto alternate = live;
    alternate.offerSource = wrongOwner;
    CHECK(core.Begin(wrongOwner, otherRegion, live.basis, alternate, {otherPane}, sentinel) == Result::Busy);
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, sentinel) == Result::Busy);
    CHECK(core.Query(wrongOwner, owner.token, live.basis, sentinel) == Result::NotFound);
    CHECK(core.Query(newSurface, owner.token, live.basis, sentinel) == Result::NotFound);
    CHECK(sentinel.token == 777 && sentinel.revision == 888);
    CHECK(core.Replace(wrongOwner, owner.token, owner.revision, live.basis, live, {nativeYield}) == Result::NotFound);
    CHECK(core.Replace(controller, owner.token, owner.revision, live.basis, live, {pluginYield}) == Result::NotOffered);
    CHECK(core.Replace(controller, owner.token, owner.revision, {10, 23}, live, {nativeYield}) == Result::Stale);
    CHECK(core.Replace(controller, owner.token, owner.revision, live.basis, live, {nativeYield}) == Result::Ok);
    CHECK(core.Replace(controller, owner.token, owner.revision, live.basis, live, {}) == Result::Stale);
    CHECK(core.Cancel(wrongOwner, owner.token) == Result::NotFound);
    CHECK(core.Cancel(newSurface, owner.token) == Result::NotFound);
    core.OwnerUnloaded(wrongOwner.owner);
    core.SourceLost(newSurface);
    core.RegionTakenOver(otherRegion);
    core.BasisChanged(live.basis);
    CHECK(core.EffectiveSlot(live, nativePane) == spare1);
    CHECK(core.Query(controller, owner.token, live.basis, sentinel) == Result::Ok && sentinel.revision == 2);
    return true;
}

bool InvalidationAndRetiredTokens() {
    Preview core;
    auto live = Layout();
    SessionState old, state, sentinel{777, 888};
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, old) == Result::Ok);
    CHECK(core.Replace(controller, old.token, old.revision, live.basis, live, {nativeYield}) == Result::Ok);
    auto changed = live;
    ++changed.basis.preferences;
    changed.panes[0].slot = spare2;
    // New committed state wins even before the host delivers invalidation.
    CHECK(core.EffectiveSlot(changed, nativePane) == spare2);
    CHECK(core.Query(controller, old.token, changed.basis, sentinel) == Result::Stale);
    CHECK(sentinel.token == 777 && sentinel.revision == 888);
    core.BasisChanged(changed.basis);
    CHECK(core.Cancel(controller, old.token) == Result::NotFound);
    CHECK(core.EffectiveSlot(changed, nativePane) == spare2);

    CHECK(core.Begin(controller, workspace, changed.basis, changed, {nativePane}, state) == Result::Ok);
    CHECK(state.token != old.token && state.revision == old.revision);
    CHECK(core.Replace(controller, old.token, old.revision, changed.basis, changed, {}) == Result::NotFound);
    CHECK(core.Replace(controller, state.token, state.revision, changed.basis, changed, {nativeYield}) == Result::Ok);
    core.RegionTakenOver(workspace);
    CHECK(core.Query(controller, state.token, changed.basis, state) == Result::NotFound);
    CHECK(core.EffectiveSlot(changed, nativePane) == spare2);

    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, state) == Result::Ok);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {nativeYield}) == Result::Ok);
    core.SourceLost(controller);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.Cancel(controller, state.token) == Result::NotFound);
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, state) == Result::Ok);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {nativeYield}) == Result::Ok);
    core.OwnerUnloaded(controller.owner);
    CHECK(core.EffectiveSlot(live, nativePane) == nativeOrigin);
    CHECK(core.Query(controller, state.token, live.basis, state) == Result::NotFound);

    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, state) == Result::Ok);
    CHECK(core.Replace(controller, state.token, state.revision, live.basis, live, {nativeYield}) == Result::Ok);
    ++live.basis.environment;
    const Ref newPane{nativePane.id, 2};
    live.panes[0].ref = newPane;
    live.offers[0].pane = newPane;
    core.BasisChanged(live.basis);
    CHECK(!core.EffectiveSlot(live, nativePane));
    CHECK(core.EffectiveSlot(live, newPane) == nativeOrigin);
    CHECK(core.Begin(controller, workspace, live.basis, live, {nativePane}, state) == Result::Stale);

    CHECK(core.Begin(controller, workspace, live.basis, live, {newPane}, old) == Result::Ok);
    core.Invalidate();
    Preview replacementHost;
    CHECK(replacementHost.Begin(controller, workspace, live.basis, live, {newPane}, state) == Result::Ok);
    CHECK(replacementHost.Cancel(controller, old.token) == Result::NotFound);
    CHECK(replacementHost.Query(controller, state.token, live.basis, state) == Result::Ok);
    return true;
}
}

int main() {
    if (!FullReplacementAndCancel() || !AtomicAndScoped() || !OneOwnerAndStaleRequests() ||
        !InvalidationAndRetiredTokens()) return 1;
    std::puts("workspace layout preview: 4 regression groups passed");
    return 0;
}
