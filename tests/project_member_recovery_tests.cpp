// Portable tests for the active-member plan; no journal codec, Windows I/O,
// provider callbacks or effect serialization run here.
#include "io/project_member_recovery.hpp"

#include <cstdio>
#include <cstdlib>

using namespace mikumikustudio::effect_save;
using namespace mikumikustudio::effect_save::member_recovery;

namespace {
unsigned checks = 0;
void Check(bool ok, const char* expression, unsigned line) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL at line %u: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr std::size_t Index(Member member) { return static_cast<std::size_t>(member); }
constexpr std::size_t Index(Slot slot) { return static_cast<std::size_t>(slot); }
constexpr Witness File(unsigned id, unsigned content) { return {{4, id}, {40 + content, {static_cast<std::uint8_t>(content)}}}; }

struct Fixture {
    Record record{};
    PairSeal seal{};
    Observations files{};
    DestinationPolicy destination;

    Fixture(bool sidecar, unsigned old)
        : destination(old ? DestinationPolicy::ApprovedReplacement : DestinationPolicy::CreateOnly) {
        for (std::size_t i = 0; i != MemberCount; ++i) {
            files[i].fill(Observation::Missing());
            if (!sidecar && i == Index(Member::Sidecar)) continue;
            const unsigned role = static_cast<unsigned>(i);
            record.members[i] = MemberRecord{{}, File(11 + role, 4 + role)};
            files[i][Index(Slot::StagedNew)] = Observation::Verified(record.members[i]->newFile);
            if (old & (1u << i)) {
                record.members[i]->oldFile = File(1 + role, 1 + role);
                files[i][Index(Slot::Active)] = Observation::Verified(*record.members[i]->oldFile);
            }
        }
        seal.pmm = record.members[0]->newFile.fingerprint;
        seal.emm = record.members[1]->newFile.fingerprint;
        if (sidecar) seal.sidecar = SidecarFingerprint{L"scene.pmm.mms-plugins.json", record.members[2]->newFile.fingerprint};
    }
    Observation& At(Member member, Slot slot) { return files[Index(member)][Index(slot)]; }

    // Simulate the pinned source/atomic no-replace contract. The executor
    // never examines or modifies an occupied destination beyond rejecting it.
    bool Apply(const Move& move) {
        auto& source = At(move.member, move.source);
        auto& target = At(move.member, move.destination);
        if (source.presence != Presence::Verified || !(source.witness == move.expected) ||
            target.presence != Presence::Missing) return false;
        target = source;
        source = Observation::Missing();
        return true;
    }
    void Execute(const Plan& plan, std::size_t count) {
        CHECK(count <= plan.count);
        for (std::size_t i = 0; i != count; ++i) CHECK(Apply(plan.moves[i]));
    }
    void CheckFinal(Decision decision) {
        CHECK(VerifyFinal(record, files, decision));
        for (std::size_t i = 0; i != MemberCount; ++i) {
            const auto& member = record.members[i];
            const auto& active = files[i][Index(Slot::Active)];
            if (!member || (decision == Decision::Rollback && !member->oldFile)) {
                CHECK(active.presence == Presence::Missing);
            } else {
                const auto& expected = decision == Decision::Forward ? member->newFile : *member->oldFile;
                CHECK(active.presence == Presence::Verified && active.witness == expected);
            }
        }
    }
};

void CheckFaultsAtEveryMove() {
    for (bool sidecar : {false, true}) {
        for (unsigned old = 0; old < (sidecar ? 8u : 4u); ++old) {
            if (old && !(old & 1u)) continue; // Orphan companions get separate rejection coverage.
            Fixture initial(sidecar, old);
            CHECK(VerifyPrepared(initial.seal, initial.record, initial.files, initial.destination));
            const auto full = MakePlan(initial.record, initial.files, Decision::Forward);
            CHECK(full.status == Status::Forward && full.count >= 2 && full.count <= 6);
            CHECK(full.moves[full.count - 1].member == Member::Main);
            CHECK(full.moves[full.count - 1].destination == Slot::Active);
            initial.CheckFinal(Decision::Rollback);
            for (std::size_t i = 0; i <= full.count; ++i) {
                auto afterCrash = initial;
                afterCrash.Execute(full, i);
                CHECK(VerifyFinal(afterCrash.record, afterCrash.files, Decision::Forward) == (i == full.count));
                auto forward = afterCrash;
                const auto finish = MakePlan(forward.record, forward.files, Decision::Forward);
                CHECK(finish.status == Status::Forward && finish.count == full.count - i);
                forward.Execute(finish, finish.count);
                forward.CheckFinal(Decision::Forward);
                // A rollback append may have completed after the first crash.
                // Re-plan after each interruption during rollback as well.
                const auto rollback = MakePlan(afterCrash.record, afterCrash.files, Decision::Rollback);
                CHECK(rollback.status == Status::Rollback);
                for (std::size_t j = 0; j <= rollback.count; ++j) {
                    auto rolledBack = afterCrash;
                    rolledBack.Execute(rollback, j);
                    const auto retry = MakePlan(rolledBack.record, rolledBack.files, Decision::Rollback);
                    CHECK(retry.status == Status::Rollback && retry.count == rollback.count - j);
                    rolledBack.Execute(retry, retry.count);
                    rolledBack.CheckFinal(Decision::Rollback);
                }
            }
        }
    }
    std::puts("PASS no-replace forward/rollback convergence after every mutation, with and without sidecar");
}

void CheckFallbackAndCleanup() {
    Fixture initial(true, 7);
    const auto full = MakePlan(initial.record, initial.files, Decision::Forward);
    CHECK(full.count == 6);
    auto lostNew = initial;
    lostNew.Execute(full, 3);
    lostNew.At(Member::Main, Slot::StagedNew) = Observation::Missing();
    const auto decision = MakePlan(lostNew.record, lostNew.files, Decision::Forward);
    CHECK(decision.status == Status::NeedsDurableRollback && !decision.count);
    CHECK(!VerifyFinal(lostNew.record, lostNew.files, Decision::Forward));
    const auto rollback = MakePlan(lostNew.record, lostNew.files, Decision::Rollback);
    CHECK(rollback.status == Status::Rollback && rollback.count == 3);
    lostNew.Execute(rollback, rollback.count);
    lostNew.CheckFinal(Decision::Rollback);

    auto lostBoth = initial;
    lostBoth.Execute(full, 1);
    lostBoth.At(Member::Effect, Slot::ParkedOld) = Observation::Missing();
    lostBoth.At(Member::Main, Slot::StagedNew) = Observation::Missing();
    auto plan = MakePlan(lostBoth.record, lostBoth.files, Decision::Forward);
    CHECK(plan.status == Status::Conflict && !plan.count);
    plan = MakePlan(lostBoth.record, lostBoth.files, Decision::Rollback);
    CHECK(plan.status == Status::Conflict && !plan.count);

    for (auto decision : {Decision::Forward, Decision::Rollback}) {
        auto cleaned = initial;
        cleaned.Execute(full, full.count);
        if (decision == Decision::Rollback) {
            const auto undo = MakePlan(cleaned.record, cleaned.files, decision);
            cleaned.Execute(undo, undo.count);
        }
        for (auto& member : cleaned.files)
            for (auto slot : {Slot::StagedNew, Slot::ParkedOld, Slot::ParkedNew}) {
                member[Index(slot)] = Observation::Missing();
                cleaned.CheckFinal(decision); // E.g., restart during verified cleanup.
            }
        cleaned.At(Member::Effect, Slot::ParkedNew) = Observation::Verified(File(88, 8));
        CHECK(!VerifyFinal(cleaned.record, cleaned.files, decision));
    }
    std::puts("PASS fallback requires a durable rollback marker; cleanup restart does not absorb strangers");
}

void CheckRefusals() {
    Fixture initial(true, 7);
    const auto reject = [&](auto inject, Status expected) {
        auto changed = initial;
        inject(changed);
        CHECK(!VerifyPrepared(changed.seal, changed.record, changed.files, changed.destination));
        for (auto decision : {Decision::Rollback, Decision::Forward}) {
            const auto result = MakePlan(changed.record, changed.files, decision);
            CHECK(result.status == expected && !result.count);
            CHECK(!VerifyFinal(changed.record, changed.files, decision));
        }
    };
    reject([](auto& f) { f.record.members[1].reset(); }, Status::Invalid);
    reject([](auto& f) { f.record.members[0].reset(); }, Status::Invalid);
    reject([](auto& f) { f.record.members[2]->newFile.file = {}; }, Status::Invalid);
    reject([](auto& f) { f.record.members[1]->newFile.fingerprint.size = 0; }, Status::Invalid);
    reject([](auto& f) { f.record.members[0]->newFile.file = f.record.members[1]->oldFile->file; }, Status::Invalid);
    reject([](auto& f) { f.record.members[0]->oldFile.reset(); }, Status::Invalid);
    reject([](auto& f) { f.record.members[2].reset(); }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Effect, Slot::Active) = {}; }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Main, Slot::StagedNew).witness.fingerprint.sha256[0] ^= 1; }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Effect, Slot::ParkedOld) = Observation::Verified(File(79, 1)); }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Effect, Slot::ParkedNew) = f.At(Member::Effect, Slot::StagedNew); }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Effect, Slot::Active).witness.file.index += 80; }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Sidecar, Slot::ParkedOld) = f.At(Member::Sidecar, Slot::StagedNew); }, Status::Conflict);
    reject([](auto& f) { f.At(Member::Sidecar, Slot::ParkedNew) = f.At(Member::Sidecar, Slot::Active); }, Status::Conflict);
    CHECK(MakePlan(initial.record, initial.files, static_cast<Decision>(99)).status == Status::Invalid);

    auto incorrectSeal = initial.seal;
    incorrectSeal.emm.size += 1;
    CHECK(!VerifyPrepared(incorrectSeal, initial.record, initial.files, initial.destination));
    incorrectSeal = initial.seal;
    incorrectSeal.pmm.sha256[0] ^= 1;
    CHECK(!VerifyPrepared(incorrectSeal, initial.record, initial.files, initial.destination));
    incorrectSeal = initial.seal;
    incorrectSeal.sidecar.reset();
    CHECK(!VerifyPrepared(incorrectSeal, initial.record, initial.files, initial.destination));
    incorrectSeal = initial.seal;
    incorrectSeal.sidecar->file.sha256[0] ^= 1;
    CHECK(!VerifyPrepared(incorrectSeal, initial.record, initial.files, initial.destination));

    CHECK(!VerifyPrepared(initial.seal, initial.record, initial.files, DestinationPolicy::CreateOnly));
    CHECK(!VerifyPrepared(initial.seal, initial.record, initial.files, static_cast<DestinationPolicy>(99)));
    for (auto kind : {SaveKind::Incremental, SaveKind::Backup}) {
        initial.seal.request.kind = kind;
        CHECK(!VerifyPrepared(initial.seal, initial.record, initial.files, initial.destination));
        auto empty = Fixture(true, 0);
        empty.seal.request.kind = kind;
        CHECK(VerifyPrepared(empty.seal, empty.record, empty.files, empty.destination));
    }

    // Occupied EMM basenames on a create-only destination are not ours, even
    // when the bytes are exactly what this save planned to write.
    auto orphan = Fixture(false, 0);
    auto equal = orphan.record.members[1]->newFile;
    equal.file.index += 100;
    orphan.At(Member::Effect, Slot::Active) = Observation::Verified(equal);
    CHECK(!VerifyPrepared(orphan.seal, orphan.record, orphan.files, orphan.destination));
    CHECK(MakePlan(orphan.record, orphan.files, Decision::Forward).status == Status::Conflict);
    CHECK(orphan.At(Member::Effect, Slot::Active).witness == equal);
    std::puts("PASS invalid seals, orphan companions, modified digests, aliases and unavailable observations fail closed");
}

void CheckLateConflicts() {
    for (auto decision : {Decision::Forward, Decision::Rollback}) {
        Fixture initial(true, 7);
        if (decision == Decision::Rollback) {
            const auto forward = MakePlan(initial.record, initial.files, Decision::Forward);
            initial.Execute(forward, forward.count);
        }
        const auto full = MakePlan(initial.record, initial.files, decision);
        CHECK(full.count == 6);
        for (std::size_t i = 0; i != full.count; ++i) {
            auto changed = initial;
            changed.Execute(full, i);
            const auto& move = full.moves[i];
            CHECK(changed.At(move.member, move.destination).presence == Presence::Missing);
            auto equal = move.expected;
            equal.file.index += 88;
            changed.At(move.member, move.destination) = Observation::Verified(equal);
            CHECK(!changed.Apply(move));
            CHECK(changed.At(move.member, move.source).witness == move.expected);
            CHECK(changed.At(move.member, move.destination).witness == equal);
            const auto retry = MakePlan(changed.record, changed.files, decision);
            CHECK(retry.status == Status::Conflict && !retry.count);

            auto wrongSource = initial;
            wrongSource.Execute(full, i);
            wrongSource.At(move.member, move.source) = Observation::Verified(equal);
            CHECK(!wrongSource.Apply(move));
            CHECK(wrongSource.At(move.member, move.source).witness == equal);
            CHECK(wrongSource.At(move.member, move.destination).presence == Presence::Missing);
            CHECK(MakePlan(wrongSource.record, wrongSource.files, decision).status == Status::Conflict);
        }
    }
    std::puts("PASS every rename refuses a late destination or replaced source even with byte-identical contents");
}
} // namespace

int main() {
    CheckFaultsAtEveryMove();
    CheckFallbackAndCleanup();
    CheckRefusals();
    CheckLateConflicts();
    std::printf("PASS project member recovery: %u checks\n", checks);
}
