#include "workspace_layout_preview.hpp"

#include <algorithm>
#include <atomic>
#include <limits>

namespace mikumikustudio::plugins::workspace_layout {
namespace {
constexpr auto MaxRevision = (std::numeric_limits<uint64_t>::max)();

// A stale event from a destroyed host must never select a replacement host's
// session, even if its source identity and initial revision are the same.
Token ReserveToken() noexcept {
    static std::atomic<Token> last{0};
    auto previous = last.load(std::memory_order_relaxed);
    while (previous != MaxRevision) {
        if (last.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed))
            return previous + 1;
    }
    return 0;
}

template<class T>
const T* Find(const std::vector<T>& records, Ref ref) noexcept {
    const auto it = std::find_if(records.begin(), records.end(),
        [ref](const T& record) { return record.ref == ref; });
    return it != records.end() ? &*it : nullptr;
}

template<class T>
Result Missing(const std::vector<T>& records, Ref ref) noexcept {
    const auto id = ref.id;
    return std::any_of(records.begin(), records.end(),
        [id](const T& record) { return record.ref.id == id; }) ? Result::Stale : Result::NotFound;
}

Result Movable(const HostLayout& live, Ref ref, Region region, const Pane*& pane) noexcept {
    if (!ref) return Result::Invalid;
    pane = Find(live.panes, ref);
    if (!pane) return Missing(live.panes, ref);
    const auto* from = Find(live.slots, pane->slot);
    return pane->movable && from && from->region == region ? Result::Ok : Result::NotOffered;
}

bool CanYieldInto(const HostLayout& live, Ref slot, Region region) noexcept {
    const auto* found = Find(live.slots, slot);
    return found && found->region == region && found->visibleYieldDestination;
}

bool Offered(const HostLayout& live, const Move& move) noexcept {
    return std::any_of(live.offers.begin(), live.offers.end(), [&](const Move& offer) {
        return offer.pane == move.pane && offer.destination == move.destination;
    });
}

bool Vacant(const HostLayout& live, Ref slot) noexcept {
    return std::none_of(live.panes.begin(), live.panes.end(),
        [slot](const Pane& pane) { return pane.slot == slot; });
}
}

bool Preview::Owns(Source source, Token token) const noexcept {
    return active_ && active_->state.token == token && active_->source == source;
}

Result Preview::Begin(Source source, Region region, Basis expected, const HostLayout& live,
                      const std::vector<Ref>& claims, SessionState& out) noexcept {
    if (!source.owner || !source.surface || !region || !expected.environment ||
        !expected.preferences || claims.empty() || claims.size() > MaxClaims)
        return Result::Invalid;
    if (expected != live.basis) return Result::Stale;
    if (!(source == live.offerSource)) return Result::NotOffered;
    if (active_) return Result::Busy;

    Session next{source, region, expected, {}};
    for (const auto ref : claims) {
        const Pane* pane = nullptr;
        if (const auto status = Movable(live, ref, region, pane); status != Result::Ok)
            return status;
        const auto end = next.claims.begin() + next.claimCount;
        if (std::find(next.claims.begin(), end, ref) != end) return Result::Invalid;
        const auto permitted = std::any_of(live.offers.begin(), live.offers.end(), [&](const Move& offer) {
            return offer.pane == ref && offer.destination != pane->slot &&
                CanYieldInto(live, offer.destination, region) && Vacant(live, offer.destination);
        });
        if (!permitted) return Result::NotOffered;
        next.claims[next.claimCount++] = ref;
    }

    const auto token = ReserveToken();
    if (!token) return Result::Exhausted;
    next.state = {token, 1};
    active_ = next;
    out = next.state;
    return Result::Ok;
}

Result Preview::Query(Source source, Token token, Basis live, SessionState& out) const noexcept {
    if (!Owns(source, token)) return Result::NotFound;
    if (active_->basis != live) return Result::Stale;
    out = active_->state;
    return Result::Ok;
}

Result Preview::Replace(Source source, Token token, uint64_t expectedRevision, Basis expected,
                        const HostLayout& live, const std::vector<Move>& moves) noexcept {
    if (!Owns(source, token)) return Result::NotFound;
    auto& session = *active_;
    if (expected != live.basis || expected != session.basis || expectedRevision != session.state.revision)
        return Result::Stale;
    if (!(source == live.offerSource)) return Result::NotOffered;
    if (moves.size() > session.claimCount) return Result::Invalid;
    if (session.state.revision == MaxRevision) return Result::Exhausted;

    for (size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        if (!move.destination) return Result::Invalid;
        const Pane* pane = nullptr;
        if (const auto status = Movable(live, move.pane, session.region, pane); status != Result::Ok)
            return status;
        const auto claimEnd = session.claims.begin() + session.claimCount;
        if (std::find(session.claims.begin(), claimEnd, move.pane) == claimEnd)
            return Result::NotOffered;
        const auto* to = Find(live.slots, move.destination);
        if (!to) return Missing(live.slots, move.destination);
        if (to->region != session.region || !to->visibleYieldDestination ||
            pane->slot == to->ref || !Offered(live, move))
            return Result::NotOffered;
        if (!Vacant(live, to->ref)) return Result::Conflict;
        for (size_t earlier = 0; earlier < i; ++earlier) {
            if (moves[earlier].pane == move.pane) return Result::Invalid;
            if (moves[earlier].destination == move.destination) return Result::Conflict;
        }
    }

    // Replace only once the complete plan is valid. No previous preview is
    // used to find a vacant destination or carried into the new plan.
    std::copy(moves.begin(), moves.end(), session.moves.begin());
    session.moveCount = moves.size();
    ++session.state.revision;
    return Result::Ok;
}

Result Preview::Cancel(Source source, Token token) noexcept {
    if (!Owns(source, token)) return Result::NotFound;
    Invalidate();
    return Result::Ok;
}

void Preview::BasisChanged(Basis live) noexcept {
    if (active_ && active_->basis != live) Invalidate();
}

void Preview::RegionTakenOver(Region region) noexcept {
    if (active_ && active_->region == region) Invalidate();
}

void Preview::SourceLost(Source source) noexcept {
    if (active_ && active_->source == source) Invalidate();
}

void Preview::OwnerUnloaded(OwnerInstance owner) noexcept {
    if (active_ && active_->source.owner == owner) Invalidate();
}

void Preview::Invalidate() noexcept { active_.reset(); }

Ref Preview::EffectiveSlot(const HostLayout& live, Ref ref) const noexcept {
    const auto* pane = Find(live.panes, ref);
    if (!pane) return {};
    if (active_ && active_->basis == live.basis) {
        const auto end = active_->moves.begin() + active_->moveCount;
        const auto moved = std::find_if(active_->moves.begin(), end,
            [ref](const Move& move) { return move.pane == ref; });
        if (moved != end) return moved->destination;
    }
    return pane->slot;
}

}  // namespace mikumikustudio::plugins::workspace_layout
