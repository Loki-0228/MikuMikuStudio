#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mikumikustudio::plugins::workspace_layout {

// Internal host model; none of these IDs or types cross the plugin ABI.
using Region = uint64_t;
using OwnerInstance = uint64_t;
using Token = uint64_t;

struct Ref {
    uint64_t id = 0, generation = 0;
    explicit operator bool() const noexcept { return id && generation; }
    friend bool operator==(Ref a, Ref b) noexcept {
        return a.id == b.id && a.generation == b.generation;
    }
    friend bool operator!=(Ref a, Ref b) noexcept { return !(a == b); }
};

struct Source {
    OwnerInstance owner = 0;
    Ref surface;
    friend bool operator==(Source a, Source b) noexcept {
        return a.owner == b.owner && a.surface == b.surface;
    }
};

struct Basis {
    uint64_t environment = 0, preferences = 0;
    friend bool operator==(Basis a, Basis b) noexcept {
        return a.environment == b.environment && a.preferences == b.preferences;
    }
    friend bool operator!=(Basis a, Basis b) noexcept { return !(a == b); }
};

struct Pane { Ref ref, slot; bool movable = false; };
struct Slot { Ref ref; Region region = 0; bool visibleYieldDestination = false; };
struct Move { Ref pane, destination; };

struct HostLayout {
    Basis basis;
    std::vector<Pane> panes;
    std::vector<Slot> slots;
    // Exact authorized pane/slot pairs for the current source; use the host's
    // existing policy, not the list of observable or geometrically nearby panes.
    Source offerSource;
    std::vector<Move> offers;
};

enum class Result { Ok, Invalid, NotFound, NotOffered, Stale, Busy, Conflict, Exhausted };
struct SessionState { Token token = 0; uint64_t revision = 0; };

// Same-region temporary YIELD to a visible, vacant host slot only. This does
// not arbitrate legacy layout sessions, write preferences or position windows.
// One instance belongs to the host's UI thread. The future adapter verifies
// the user gesture, supplies the live host layout at each safe point, and
// invalidates on lifecycle events. A changed layout/offer set requires a new
// basis, even when the same pane or slot ID is reused. The adapter must also
// prevent Begin while an affected region is held by an existing interaction.
class Preview {
public:
    static constexpr size_t MaxClaims = 8;

    Preview() = default;
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    Result Begin(Source source, Region region, Basis expected, const HostLayout& live,
                 const std::vector<Ref>& claims, SessionState& out) noexcept;
    Result Query(Source source, Token token, Basis live, SessionState& out) const noexcept;
    Result Replace(Source source, Token token, uint64_t expectedRevision, Basis expected,
                   const HostLayout& live, const std::vector<Move>& moves) noexcept;
    Result Cancel(Source source, Token token) noexcept;

    void BasisChanged(Basis live) noexcept;
    void RegionTakenOver(Region region) noexcept;
    void SourceLost(Source source) noexcept;
    void OwnerUnloaded(OwnerInstance owner) noexcept;
    void Invalidate() noexcept;

    // Composes directly with the current committed pane map. A stale overlay
    // cannot be projected and no start-of-session placement is replayed.
    Ref EffectiveSlot(const HostLayout& live, Ref pane) const noexcept;

private:
    struct Session {
        Source source;
        Region region;
        Basis basis;
        SessionState state;
        std::array<Ref, MaxClaims> claims{};
        std::array<Move, MaxClaims> moves{};
        size_t claimCount = 0, moveCount = 0;
    };

    bool Owns(Source source, Token token) const noexcept;
    std::optional<Session> active_;
};

}  // namespace mikumikustudio::plugins::workspace_layout
