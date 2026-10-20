#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/model.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

namespace { thread_local long allocationLimit = -1; }
void* operator new(std::size_t size) {
    if (allocationLimit == 0) throw std::bad_alloc();
    if (allocationLimit > 0) --allocationLimit;
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
namespace {
using namespace mikumikustudio;
using namespace text_encoding;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using Targets = std::vector<std::pair<const mdl::DisplayGroup*, DisplayGroupTextState>>;
DisplayGroupTextState Capture(const mdl::ModelRecord& model, const mdl::DisplayGroup& group) {
    DisplayGroupTextState state; Check(CaptureDisplayGroupText(model, group, state), "capture exact sidecar"); return state;
}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<mdl::ModelRecord> other = std::make_unique<mdl::ModelRecord>();
    mdl::DisplayGroup first{}, second{}, unrelated{};
    Fixture() {
        std::strcpy(first.name, "first"); std::strcpy(first.nameEn, "first-en");
        std::strcpy(second.name, "second"); std::strcpy(second.nameEn, "second-en");
        std::strcpy(unrelated.name, "other");
        SetDisplayGroupName(*model, first, L"原始分组完整名称", L"Original English group");
        SetDisplayGroupName(*other, unrelated, L"无关分组", L"Unrelated group");
    }
    ~Fixture() { ForgetDisplayGroupNames(*model); ForgetDisplayGroupNames(*other); }
};
void ExchangeAndAbsence() {
    Fixture f;
    const auto original = Capture(*f.model, f.first), absent = Capture(*f.model, f.second);
    Check(original.present && !absent.present, "absence differs from an empty named record");
    auto desired = original; desired.names[1] = L"Updated English with 完整 Unicode";
    auto added = desired; added.legacy = {"second", "second-en"};
    PreparedDisplayGroupTextBatch prepared;
    Check(prepared.Prepare(*f.model, {{&f.first, {}}, {&f.second, added}}), "prepare remove and add atomically");
    Check(prepared.IsCurrent() && prepared.RetainedBytes() > sizeof(prepared), "prepared source and memory accounting");
    Check(MatchesDisplayGroupText(*f.model, f.first, original) && MatchesDisplayGroupText(*f.model, f.second, absent), "preparation leaves the live map unchanged");
    SetDisplayGroupName(*f.other, f.unrelated, L"其他分组独立更新", L"Independent newer value");
    const auto unrelated = Capture(*f.other, f.unrelated);
    Check(prepared.IsCurrent(), "unrelated model edits do not stale or get overwritten");
    const auto bytes = prepared.RetainedBytes();
    allocationLimit = 0; prepared.Publish(); allocationLimit = -1;
    Check(!prepared.IsCurrent() && prepared.RetainedBytes() == bytes, "publication consumes the candidate without allocating");
    Check(MatchesDisplayGroupText(*f.model, f.first, {}) && MatchesDisplayGroupText(*f.model, f.second, added), "both prepared targets publish");
    Check(MatchesDisplayGroupText(*f.other, f.unrelated, unrelated), "publication preserves unrelated newer metadata");
    Check(DisplayGroupName(f.first, true) == L"first-en" && DisplayGroupName(f.second, true) == added.names[1], "native fallback and full Unicode display remain correct");
    PreparedDisplayGroupTextBatch restore;
    Check(restore.Prepare(*f.model, {{&f.first, original}, {&f.second, absent}}), "prepare exact undo including absence");
    allocationLimit = 0; restore.Publish(); allocationLimit = -1;
    Check(MatchesDisplayGroupText(*f.model, f.first, original) && MatchesDisplayGroupText(*f.model, f.second, absent), "undo restores exact original records");
    { PreparedDisplayGroupTextBatch cancelled; Check(cancelled.Prepare(*f.model, {{&f.first, desired}}), "prepare cancellable update"); }
    Check(MatchesDisplayGroupText(*f.model, f.first, original), "dropping the candidate changes nothing");
}
void InvalidAndStale() {
    Fixture f; const auto original = Capture(*f.model, f.first);
    auto desired = original; desired.names[1] = L"desired";
    PreparedDisplayGroupTextBatch batch;
    Check(!batch.IsCurrent() && !batch.RetainedBytes(), "empty wrapper is not ready");
    Check(!batch.Prepare(*f.model, {{nullptr, desired}}), "null target rejected");
    Check(!batch.Prepare(*f.model, {{&f.first, desired}, {&f.first, {}}}), "duplicate target rejected");
    Check(!batch.Prepare(*f.model, {{&f.unrelated, desired}}), "foreign owner rejected");
    DisplayGroupTextState output = desired;
    Check(!CaptureDisplayGroupText(*f.model, f.unrelated, output) && output.names == desired.names && output.present == desired.present, "failed capture leaves output untouched");
    Check(batch.Prepare(*f.model, {{&f.first, desired}}) && batch.IsCurrent(), "invalid attempts leave wrapper reusable");
    Check(!batch.Prepare(*f.model, {}) && batch.IsCurrent(), "repreparation cannot mutate an existing candidate");
    SetDisplayGroupName(*f.model, f.first, L"changed source", L"newer source");
    Check(!batch.IsCurrent(), "same group source update invalidates prepared text");
    Check(!MatchesDisplayGroupText(*f.model, f.first, original), "exact source match notices replaced contents");
    PreparedDisplayGroupTextBatch current;
    Check(current.Prepare(*f.model, {{&f.second, desired}}), "capture an absent group");
    SetDisplayGroupName(*f.other, f.second, L"foreign insertion", L"foreign");
    Check(!current.IsCurrent(), "insertion with foreign ownership invalidates absence");
    PreparedDisplayGroupTextBatch moved;
    Check(moved.Prepare(*f.model, {{&f.first, desired}}), "prepare move ownership");
    PreparedDisplayGroupTextBatch target(std::move(moved));
    Check(!moved.IsCurrent() && !moved.RetainedBytes() && target.IsCurrent(), "move preserves one candidate owner");
}
void AllocationFailures() {
    Fixture f; const auto original = Capture(*f.model, f.first), absent = Capture(*f.model, f.second);
    auto desired = original; desired.names[0] = std::wstring(800, L'中'); desired.names[1] = std::wstring(900, L'x');
    desired.legacy = {std::string(400, 'n'), std::string(500, 'e')};
    const Targets targets{{&f.first, desired}, {&f.second, desired}};
    unsigned failures = 0; bool passed = false;
    for (long limit = 0; limit < 128 && !passed; ++limit) {
        PreparedDisplayGroupTextBatch candidate;
        allocationLimit = limit;
        try { passed = candidate.Prepare(*f.model, targets); allocationLimit = -1; }
        catch (const std::bad_alloc&) { allocationLimit = -1; ++failures; }
        Check(MatchesDisplayGroupText(*f.model, f.first, original) && MatchesDisplayGroupText(*f.model, f.second, absent), "allocation refusal never partially changes metadata");
        if (!passed) Check(!candidate.IsCurrent() && !candidate.RetainedBytes(), "failed preparation retains no candidate");
        else {
            Check(candidate.IsCurrent() && candidate.RetainedBytes() >= 2 * (800 + 900) * sizeof(wchar_t), "full Unicode capacity is charged");
            allocationLimit = 0; candidate.Publish(); allocationLimit = -1;
            Check(MatchesDisplayGroupText(*f.model, f.first, desired) && MatchesDisplayGroupText(*f.model, f.second, desired), "retry after allocation failures publishes complete targets");
        }
    }
    Check(passed && failures >= 8, "exercise preparation allocation boundaries");
}
}
int main() {
    static_assert(!std::is_copy_constructible_v<PreparedDisplayGroupTextBatch>);
    static_assert(std::is_nothrow_move_constructible_v<PreparedDisplayGroupTextBatch>);
    ExchangeAndAbsence(); InvalidAndStale(); AllocationFailures();
    std::printf("PASS display group text: %u checks\n", checks);
}
