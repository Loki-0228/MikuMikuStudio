#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mikumikustudio::effect_save::transaction {

// SAVE-02 publication policy. These indices are internal, not an EMM or
// plugin JSON format. An original-profile transaction has only PMM and EMM.
enum Member : std::size_t { Main, Effect, Plugin, MemberCount };
using Hashes = std::array<std::string, MemberCount>;

struct Generation {
    Hashes fresh, previous;
    bool withPlugin = false;
    bool Contains(std::size_t i) const noexcept { return i != Plugin || withPlugin; }
};

struct Observation {
    Hashes active, staged, displaced;
};

enum class Action { DisplacePrevious, InstallFresh, RemoveFresh, RestorePrevious };
struct Move { Action action; Member member; };
struct Plan { bool forward; std::vector<Move> moves; };

// Validate the entire generation before performing any mutation. A wrong
// digest is interference, not a missing stage; keep the journal and every
// remaining member intact for explicit recovery. Absence is recoverable only
// when one whole recorded generation is still available.
inline Plan Resolve(const Generation& generation, const Observation& observed, bool preferForward) {
    const auto& fresh = generation.fresh;
    const auto& previous = generation.previous;
    const auto& active = observed.active;
    const auto& staged = observed.staged;
    const auto& displaced = observed.displaced;
    for (std::size_t i = 0; i < MemberCount; ++i) {
        if (!generation.Contains(i)) {
            if (!fresh[i].empty() || !previous[i].empty() || !active[i].empty() ||
                !staged[i].empty() || !displaced[i].empty())
                throw std::invalid_argument("unexpected frozen project member");
            continue;
        }
        if (fresh[i].empty()) throw std::invalid_argument("missing frozen member hash");
        if ((!active[i].empty() && active[i] != fresh[i] && active[i] != previous[i]) ||
            (!staged[i].empty() && staged[i] != fresh[i]) ||
            (!displaced[i].empty() && displaced[i] != previous[i]))
            throw std::runtime_error("frozen project member changed outside interrupted save");
        // Publication consumes its fresh stage by rename, and displacement
        // consumes the active old member. Equal bytes at both names cannot
        // prove who created the active name; refuse even an identical arrival.
        if ((!staged[i].empty() && active[i] == fresh[i] && fresh[i] != previous[i]) ||
            (!displaced[i].empty() && active[i] == previous[i]))
            throw std::runtime_error("duplicate frozen project member outside interrupted save");
    }
    const auto candidate = [&](bool forward) -> std::optional<Plan> {
        Plan result{forward, {}};
        // Install the discoverable PMM entry last; no reader may load any
        // member while the journal is present, regardless of this ordering.
        constexpr std::array<Member, MemberCount> order{Effect, Plugin, Main};
        for (const auto member : order) {
            const std::size_t i = member;
            if (!generation.Contains(i)) continue;
            if (forward) {
                if (active[i] == fresh[i]) continue;
                if (staged[i] != fresh[i]) return {};
                if (!active[i].empty()) {
                    if (!displaced[i].empty()) return {};
                    result.moves.push_back({Action::DisplacePrevious, member});
                }
                result.moves.push_back({Action::InstallFresh, member});
            } else {
                if (active[i] == previous[i]) continue;
                if (!previous[i].empty() && displaced[i] != previous[i]) return {};
                if (!active[i].empty()) result.moves.push_back({Action::RemoveFresh, member});
                if (!previous[i].empty()) result.moves.push_back({Action::RestorePrevious, member});
            }
        }
        return result;
    };
    if (preferForward) {
        if (auto plan = candidate(true)) return *plan;
    }
    // A persisted rollback decision never flips to forward, even if only the
    // fresh generation remains.
    if (auto plan = candidate(false)) return *plan;
    throw std::runtime_error("no complete frozen project generation is recoverable");
}

} // namespace mikumikustudio::effect_save::transaction
