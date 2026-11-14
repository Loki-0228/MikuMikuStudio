#pragma once

#include "effect_save_pair.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mikumikustudio::effect_save::member_recovery {

// Active members of a future extension of the existing project journal.
// This planner has no I/O, journal codec, archive moves, or production wiring.
enum class Member : std::size_t { Main, Effect, Sidecar };
enum class Slot : std::size_t { Active, StagedNew, ParkedOld, ParkedNew };
inline constexpr std::size_t MemberCount = 3;
inline constexpr std::size_t SlotCount = 4;

// The Win32 adapter must obtain the identity and fingerprint from the same
// pinned regular-file handle. A reparse point, directory, hard link or an
// unverified I/O result must be reported as unavailable, never missing.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t index = 0;
    bool operator==(const FileIdentity& other) const noexcept {
        return volume == other.volume && index == other.index;
    }
};

struct Witness {
    FileIdentity file;
    ArtifactFingerprint fingerprint;
    bool operator==(const Witness& other) const noexcept {
        return file == other.file && fingerprint == other.fingerprint;
    }
};

enum class Presence { Unavailable, Missing, Verified };
struct Observation {
    Presence presence = Presence::Unavailable;
    Witness witness{};
    static constexpr Observation Missing() noexcept { return {Presence::Missing, {}}; }
    static constexpr Observation Verified(Witness file) noexcept { return {Presence::Verified, file}; }
};

struct MemberRecord {
    // No old active file is valid for Save As/numbered backups and for an
    // individual companion of an existing project. New files are required.
    std::optional<Witness> oldFile;
    Witness newFile;
};

struct Record {
    std::array<std::optional<MemberRecord>, MemberCount> members;
};

using MemberFiles = std::array<Observation, SlotCount>;
using Observations = std::array<MemberFiles, MemberCount>;

enum class Decision { Rollback, Forward };
enum class DestinationPolicy { CreateOnly, ApprovedReplacement };
// The journal decoder supplies the last complete, durably flushed decision.
// An interrupted append never grants forward publication. A rollback plan
// selected after a forward decision is usable only after its own durable append.
enum class Status { Invalid, Conflict, NeedsDurableRollback, Rollback, Forward };

struct Move {
    Member member;
    Slot source;
    Slot destination;
    Witness expected;
    // All moves require this exact pinned source and an empty destination.
    // Use a no-replace handle rename, even at Active; never delete by pathname.
};

struct Plan {
    Status status = Status::Invalid;
    std::array<Move, MemberCount * 2> moves{};
    std::size_t count = 0;
};

namespace detail {
inline bool ValidIdentity(const FileIdentity& file) noexcept { return file.index != 0; }

inline bool Valid(const Record& record) noexcept {
    if (!record.members[0] || !record.members[1]) return false;
    // A companion alone does not make the basename ours to overwrite.
    if (!record.members[0]->oldFile &&
        (record.members[1]->oldFile || (record.members[2] && record.members[2]->oldFile))) return false;
    std::array<FileIdentity, MemberCount * 2> identities{};
    std::size_t count = 0;
    const auto add = [&](const Witness& file) {
        if (!ValidIdentity(file.file)) return false;
        for (std::size_t i = 0; i != count; ++i)
            if (identities[i] == file.file) return false;
        identities[count++] = file.file;
        return true;
    };
    for (const auto& member : record.members) {
        if (!member) continue;
        if (!member->newFile.fingerprint.size || !add(member->newFile) ||
            (member->oldFile && !add(*member->oldFile))) return false;
    }
    return true;
}

inline bool Valid(const Record& record, const Observations& files) noexcept {
    for (std::size_t i = 0; i != MemberCount; ++i) {
        const auto& member = record.members[i];
        const auto& slots = files[i];
        unsigned seenOld = 0, seenNew = 0;
        for (std::size_t j = 0; j != SlotCount; ++j) {
            const auto& observation = slots[j];
            if (observation.presence == Presence::Missing) continue;
            if (observation.presence != Presence::Verified || !member) return false;
            const auto slot = static_cast<Slot>(j);
            const bool old = member->oldFile && observation.witness == *member->oldFile;
            const bool fresh = observation.witness == member->newFile;
            if (old && (slot == Slot::Active || slot == Slot::ParkedOld) && ++seenOld == 1) continue;
            if (fresh && slot != Slot::ParkedOld && ++seenNew == 1) continue;
            return false;
        }
    }
    return true;
}

inline bool Is(const MemberFiles& files, Slot slot, const Witness& witness) noexcept {
    const auto& observation = files[static_cast<std::size_t>(slot)];
    return observation.presence == Presence::Verified && observation.witness == witness;
}
inline bool Missing(const MemberFiles& files, Slot slot) noexcept {
    return files[static_cast<std::size_t>(slot)].presence == Presence::Missing;
}

inline bool CanFinish(const Record& record, const Observations& files, Decision decision) noexcept {
    for (std::size_t i = 0; i != MemberCount; ++i) {
        if (!record.members[i]) continue;
        const auto& member = *record.members[i];
        const auto& slots = files[i];
        if (decision == Decision::Forward) {
            if (!Is(slots, Slot::Active, member.newFile) && !Is(slots, Slot::StagedNew, member.newFile)) return false;
        } else if (member.oldFile && !Is(slots, Slot::Active, *member.oldFile) &&
                   !Is(slots, Slot::ParkedOld, *member.oldFile)) return false;
    }
    return true;
}
} // namespace detail

// Live start only, before publication of the existing canonical journal.
// Closed stages must match the immutable worker seal and all paths must be
// resolved, distinct and same-volume. The adapter owns those filesystem
// checks, the approved prior companion association and archive setup. A
// create-only destination can never be converted to a replacement by copying
// whatever appeared at the target into the record.
inline bool VerifyPrepared(const PairSeal& seal, const Record& record, const Observations& files,
    DestinationPolicy destination) noexcept {
    if (!detail::Valid(record) || !detail::Valid(record, files) ||
        !(record.members[0]->newFile.fingerprint == seal.pmm) ||
        !(record.members[1]->newFile.fingerprint == seal.emm) ||
        bool(record.members[2]) != bool(seal.sidecar) ||
        (seal.sidecar && !(record.members[2]->newFile.fingerprint == seal.sidecar->file))) return false;
    if (destination != DestinationPolicy::CreateOnly && destination != DestinationPolicy::ApprovedReplacement) return false;
    if ((seal.request.kind == SaveKind::Incremental || seal.request.kind == SaveKind::Backup) &&
        destination != DestinationPolicy::CreateOnly) return false;
    for (std::size_t i = 0; i != MemberCount; ++i) {
        if (!record.members[i]) continue;
        const auto& member = *record.members[i];
        const auto& slots = files[i];
        if ((destination == DestinationPolicy::CreateOnly && member.oldFile) ||
            !detail::Missing(slots, Slot::ParkedOld) || !detail::Missing(slots, Slot::ParkedNew) ||
            !detail::Is(slots, Slot::StagedNew, member.newFile) ||
            (member.oldFile ? !detail::Is(slots, Slot::Active, *member.oldFile) : !detail::Missing(slots, Slot::Active)))
            return false;
    }
    return true;
}

// The active project lock and archive preflight remain the caller's job.
// Before the first mutation, the caller must recheck all supplied observations
// and pin verified sources as appropriate. The observed state is a dry run;
// a failed rename or failed final check leaves the canonical journal in place.
// Verify all final members and the existing archive, then clean up by pinned
// identity/digest and remove the journal last. Only then report disk outcome.
inline Plan MakePlan(const Record& record, const Observations& files, Decision decision) noexcept {
    if ((decision != Decision::Rollback && decision != Decision::Forward) || !detail::Valid(record)) return {};
    if (!detail::Valid(record, files)) return {Status::Conflict};
    if (!detail::CanFinish(record, files, decision)) {
        if (decision == Decision::Forward && detail::CanFinish(record, files, Decision::Rollback))
            return {Status::NeedsDurableRollback}; // No mutations under this result.
        return {Status::Conflict};
    }

    Plan plan{decision == Decision::Forward ? Status::Forward : Status::Rollback};
    const auto add = [&](std::size_t i, Slot source, Slot destination, const Witness& file) {
        plan.moves[plan.count++] = {static_cast<Member>(i), source, destination, file};
    };
    // Sidecars and EMMs before PMM: a newly installed PMM is never the first
    // public new member. Save/open must still reconcile the journal first.
    for (const auto i : {std::size_t{1}, std::size_t{2}, std::size_t{0}}) {
        if (!record.members[i]) continue;
        const auto& member = *record.members[i];
        const auto& slots = files[i];
        if (decision == Decision::Forward) {
            if (detail::Is(slots, Slot::Active, member.newFile)) continue;
            if (!detail::Missing(slots, Slot::Active))
                add(i, Slot::Active, Slot::ParkedOld, *member.oldFile);
            add(i, Slot::StagedNew, Slot::Active, member.newFile);
        } else {
            if (member.oldFile && detail::Is(slots, Slot::Active, *member.oldFile)) continue;
            if (!detail::Missing(slots, Slot::Active))
                add(i, Slot::Active, Slot::ParkedNew, member.newFile);
            if (member.oldFile) add(i, Slot::ParkedOld, Slot::Active, *member.oldFile);
        }
    }
    return plan;
}

// A separate observation from disk is required after applying a plan. Missing
// auxiliary stages after interrupted cleanup is allowed; unknown occupants
// or a mismatched final always retain the journal and forbid a success receipt.
inline bool VerifyFinal(const Record& record, const Observations& files, Decision decision) noexcept {
    const auto plan = MakePlan(record, files, decision);
    return !plan.count && (plan.status == Status::Rollback || plan.status == Status::Forward);
}

} // namespace mikumikustudio::effect_save::member_recovery
