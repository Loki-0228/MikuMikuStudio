// MAINTENANCE: portable coverage of autosave's timestamp ranking and rotation plan.

#include "app/autosave_rotation.hpp"
#include "io/project_recovery_journal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace rotation = mikumikustudio::autosave::rotation;
using Paths = std::vector<fs::path>;
using Time = fs::file_time_type;
using Action = rotation::Action;

unsigned checks = 0;
void Check(bool good, const char* reason) {
    ++checks;
    if (!good) throw std::runtime_error(reason);
}

void CheckSnapshotOrder() {
    const Paths expected{"c.pmm", "a.pmm", "b.pmm", "d.pmm"};
    Paths paths{"a.pmm", "b.pmm", "c.pmm", "d.pmm"};
    do {
        std::map<fs::path, unsigned> calls;
        const auto ranked = rotation::Rank(paths, [&](const fs::path& path, std::error_code&) {
            // A second read of the same file would return a different value.
            if (++calls[path] > 1) return Time::max();
            if (path == "c.pmm") return Time::min();
            return Time{};
        });
        Check(ranked.complete && ranked.paths == expected,
            "mtime wins across names; equal times use native lexical order regardless of enumeration");
        Check(calls.size() == paths.size() &&
            std::all_of(calls.begin(), calls.end(), [](const auto& item) { return item.second == 1; }),
            "ranking reads each candidate exactly once");
    } while (std::next_permutation(paths.begin(), paths.end()));
}

void CheckCapacityBoundary() {
    const Paths paths{"a.pmm", "b.pmm", "c.pmm"};
    unsigned calls = 0;
    const auto readTime = [&](const fs::path& path, std::error_code&) {
        ++calls;
        return path == "c.pmm" ? Time::min() : Time{};
    };
    const auto room = rotation::Prepare(paths, 4, true, readTime);
    Check(room.action == Action::Append && room.CanWrite() && room.oldestFirst.empty(),
        "below capacity, writing requires no removal");
    const auto disabled = rotation::Prepare(paths, 3, false, readTime);
    Check(disabled.action == Action::Full && !disabled.CanWrite() && disabled.oldestFirst.empty(),
        "at capacity with overwrite off, refuse without exposing a removal candidate");
    Check(calls == 0, "no mtime reads outside the overwrite decision");
    const Paths expected{"c.pmm", "a.pmm", "b.pmm"};
    for (unsigned limit : {3u, 1u}) {
        calls = 0;
        const auto ready = rotation::Prepare(paths, limit, true, readTime);
        Check(ready.action == Action::Rotate && ready.CanWrite() && ready.oldestFirst == expected,
            "at or above capacity, supply the complete oldest-first list for rotation");
        Check(calls == paths.size(), "preparing rotation takes one mtime read per candidate");
    }
}

class ScratchFolder {
public:
    ScratchFolder() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = fs::temp_directory_path();
        for (unsigned i = 0; i < 100; ++i) {
            const auto candidate = base / ("mms-autosave-rotation-" + std::to_string(stamp) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a private fixture folder");
    }
    ~ScratchFolder() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    fs::path path;
};

void CheckJournalBoundary() {
    using mikumikustudio::project_recovery::JournalAbsent;
    using mikumikustudio::project_recovery::JournalPath;
    const ScratchFolder scratch;
    const auto journal = JournalPath(scratch.path / "saved.pmm");
    Check(journal == scratch.path / "saved.pmm.mms-save.json", "journal is a sibling with the expected suffix");
    Check(JournalAbsent(journal), "there is no journal to handle");
    Check(JournalAbsent(JournalPath(scratch.path / "missing" / "saved.pmm")),
        "normal absence includes a missing parent directory");

    std::ofstream(journal) << "interrupted save";
    Check(!JournalAbsent(journal), "a regular journal requires handling");
    std::error_code error;
    const auto invalid = JournalPath(journal / "nested.pmm");
    Check(!JournalAbsent(invalid,error) && error == std::errc::not_a_directory,
        "a failed path lookup is not proven absence");
    bool reported = false;
    try { (void)JournalAbsent(invalid); }
    catch (const fs::filesystem_error& e) { reported = e.code() == error; }
    Check(reported, "recovery reports the lookup error");
    fs::remove(journal);
    fs::create_directory(journal);
    Check(!JournalAbsent(journal), "a directory at the journal path is not absence");
    fs::remove(journal);
    const auto linked = scratch.path / "other-file";
    fs::create_symlink(linked, journal);
    Check(!JournalAbsent(journal), "a dangling journal symlink is not absence");
    std::ofstream(linked) << "unrelated file";
    Check(!JournalAbsent(journal), "a live journal symlink is not absence");
    fs::remove(journal);

    Check(JournalAbsent(journal), "normal operation can resume once the journal is gone");
}

void CheckUnavailableAge() {
    const ScratchFolder scratch;
    const auto a = scratch.path / "a.pmm", b = scratch.path / "b.pmm";
    const auto c = scratch.path / "c.pmm", d = scratch.path / "d.pmm";
    for (const auto& path : {a, b, c, d}) {
        std::ofstream fixture(path, std::ios::binary);
        fixture << path.filename().string();
        fixture.close();
        Check(static_cast<bool>(fixture), "create a disposable fixture");
    }
    const auto now = Time::clock::now();
    fs::last_write_time(a, now);
    fs::last_write_time(c, now - std::chrono::hours(1));
    // b was found by the scan, then vanishes before the timestamp probe.
    const auto moved = scratch.path / "moved.pmm";
    fs::rename(b, moved);
    std::map<fs::path, unsigned> calls;
    const auto readTime = [&](const fs::path& path, std::error_code& error) {
        ++calls[path];
        if (path == d) {
            error = std::make_error_code(std::errc::permission_denied);
            return Time::min();
        }
        return rotation::ReadWriteTime(path, error);
    };
    // In the previous pairwise comparator, a < b, b < c and c < a.
    const Paths candidates{d, b, c, a};
    const auto ranked = rotation::Rank(candidates, readTime);
    Check(!ranked.complete && ranked.paths == Paths{c, a, b, d},
        "listing keeps both failures but sorts them after known ages and breaks ties lexically");
    for (unsigned limit : {4u, 2u}) {
        calls.clear();
        const auto deferred = rotation::Prepare(candidates, limit, true, readTime);
        Check(deferred.action == Action::AgeUnavailable && !deferred.CanWrite() && deferred.oldestFirst.empty(),
            "an unknown age blocks the whole rotation before any neighboring backup is exposed for removal");
        Check(calls == (std::map<fs::path, unsigned>{{a, 1}, {b, 1}, {c, 1}, {d, 1}}),
            "both unavailable and available timestamps are observed exactly once");
    }
    Check(fs::exists(a) && fs::exists(c) && fs::exists(d) && fs::exists(moved),
        "ranking and planning leave the fixture files alone");
    fs::rename(moved, b);
    const auto retried = rotation::Prepare(candidates, 4, true, rotation::ReadWriteTime);
    Check(retried.action == Action::Rotate && retried.CanWrite() &&
        retried.oldestFirst.size() == candidates.size() && retried.oldestFirst.front() == c,
        "retry resumes normal rotation once ages are available again");
}
}

int main() {
    try {
        CheckSnapshotOrder();
        CheckCapacityBoundary();
        CheckUnavailableAge();
        CheckJournalBoundary();
        std::printf("PASS autosave rotation (%u checks)\n", checks);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL autosave rotation: %s\n", error.what());
        return 1;
    }
}
