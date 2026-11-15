#include "mikumikustudio/alembic_export.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string_view>

namespace {
using mikumikustudio::alembic_export::IndexFilter;
using mikumikustudio::alembic_export::Selection;

void Check(bool ok, const char* context) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", context);
        std::exit(1);
    }
}

// Reference: the old dialog parser, expanded only for small test inputs.
bool LegacyIndices(const std::wstring& text, std::set<std::uint32_t>& values) {
    const wchar_t* p = text.c_str();
    while (*p) {
        while (std::iswspace(*p) || *p == L',') ++p;
        if (!*p) break;
        if (!std::iswdigit(*p)) return false;
        wchar_t* end = nullptr;
        const auto first = std::wcstoul(p, &end, 10);
        p = end;
        while (std::iswspace(*p)) ++p;
        auto last = first;
        if (*p == L'-') {
            ++p;
            while (std::iswspace(*p)) ++p;
            if (!std::iswdigit(*p)) return false;
            last = std::wcstoul(p, &end, 10);
            p = end;
        }
        if (last < first || last > 16000000 || last - first > 16000000) return false;
        for (auto i = first; i <= last; ++i) values.insert(std::uint32_t(i));
        while (std::iswspace(*p)) ++p;
        if (*p && *p != L',') return false;
    }
    return true;
}

void MatchLegacy(const std::wstring& text) {
    std::set<std::uint32_t> expected;
    const bool valid = LegacyIndices(text, expected);
    IndexFilter actual{7};
    const auto fail = [&] {
        std::fprintf(stderr, "FAIL: old and new filters differ for [%ls]\n", text.c_str());
        std::exit(1);
    };
    if (IndexFilter::Parse(text, actual) != valid) fail();
    if (!valid) {
        if (actual.empty() || !actual.Allows(7) || actual.Allows(0)) fail();
        return;
    }
    if (actual.empty() != expected.empty()) fail();
    for (std::uint32_t n = 0; n <= 12; ++n)
        if (actual.Allows(n) != (expected.empty() || expected.count(n) != 0)) fail();
    for (const auto n : expected) {
        if (!actual.Allows(n)) fail();
        if (n && actual.Allows(n - 1) != (expected.count(n - 1) != 0)) fail();
        if (actual.Allows(n + 1) != (expected.count(n + 1) != 0)) fail();
    }
}

void MatchShortInputs(std::wstring& text, unsigned left) {
    MatchLegacy(text);
    if (left == 0) return;
    for (const auto c : std::wstring_view(L"02 ,-\t+a")) {
        text.push_back(c);
        MatchShortInputs(text, left - 1);
        text.pop_back();
    }
}

void ParsingAndMembership() {
    std::wstring text;
    MatchShortInputs(text, 5);
    MatchLegacy(L"\r\n ,6-9, 3 - 5,8,, 12, 0001,1, 5,12\t, ");
    MatchLegacy(L"0000\v-\f0, 7, 12, ");
    MatchLegacy(L"0,2-4 5"); // Whitespace does not replace a comma between terms.
    MatchLegacy(L"1,2x");
    MatchLegacy(L"0x10");
    MatchLegacy(L"5-4");
    MatchLegacy(L"1,16000001");
    MatchLegacy(L"1,999999999999999999999999999999999999999999999");

    IndexFilter filter;
    Check(filter.empty() && filter.Allows(0) && filter.Allows(UINT32_MAX), "default allows all");
    Check(IndexFilter::Parse(L"9-12,3-4,8-9,5-7,1, 1, 4", filter) &&
        filter.RangeCount() == 2, "sort and merge adjacent, duplicate, and overlapping intervals");
    Check(filter.Allows(1) && !filter.Allows(2) && filter.Allows(3) &&
        filter.Allows(12) && !filter.Allows(13), "preserve gaps and include both endpoints");
    Check(IndexFilter::Parse(L",, \t, \r\n", filter) && filter.empty() &&
        filter.Allows(16000001), "separator-only input selects all and clears the prior filter");
}

void LargeRanges() {
    IndexFilter filter;
    Check(IndexFilter::Parse(L"0 - 16000000", filter) && filter.RangeCount() == 1,
        "the entire supported interval is one stored range, not 16 million indices");
    Check(filter.Allows(0) && filter.Allows(16000000) && !filter.Allows(16000001),
        "the largest supported index is included without widening");
    Check(IndexFilter::Parse(L"15000000-16000000, 3000000-9000000, 1000000-4000000,"
        L"9000001-12000000, 14000000-15999999, 4000000", filter) && filter.RangeCount() == 2,
        "large unsorted ranges merge without filling the gap");
    for (std::uint32_t n = 0; n <= 16000000; n += 53) {
        const bool expected = (n >= 1000000 && n <= 12000000) || n >= 14000000;
        Check(filter.Allows(n) == expected, "large disjoint ranges match the selected vertices");
    }
    Check(!filter.Allows(12000001) && !filter.Allows(13999999) &&
        filter.Allows(14000000) && !filter.Allows(UINT32_MAX), "large interval edges and gap");
    Check(!IndexFilter::Parse(L"0-16000001", filter) && filter.RangeCount() == 2,
        "oversized range does not replace the previous valid filter");
    Check(IndexFilter::Parse(L"16000000", filter) && filter.RangeCount() == 1 &&
        filter.Allows(16000000) && !filter.Allows(15999999), "maximum singleton is valid");
}

void NumericCallers() {
    const std::vector<std::uint32_t> vertices{UINT32_MAX, 1, 0, UINT32_MAX - 1, 1};
    Selection selection{false, 0, {3, 1, 2, 3}, vertices, true};
    Check(selection.materials.RangeCount() == 1 && selection.materials.Allows(2) &&
        !selection.materials.Allows(0), "aggregate initialization with a numeric list");
    Check(selection.vertices.RangeCount() == 2 && selection.vertices.Allows(0) &&
        !selection.vertices.Allows(2) && !selection.vertices.Allows(UINT32_MAX - 2) &&
        selection.vertices.Allows(UINT32_MAX - 1) && selection.vertices.Allows(UINT32_MAX),
        "unsorted numeric input, duplicate indices, and adjacent uint32 endpoints");
    selection.materials = {};
    selection.vertices = {0, 1, 3};
    Check(selection.materials.empty() && selection.materials.Allows(2) &&
        !selection.vertices.Allows(2) && selection.vertices.Allows(3),
        "existing list assignment uses the same membership rules");
    selection.vertices = vertices;
    Check(selection.vertices.Allows(UINT32_MAX) && !selection.vertices.Allows(3),
        "existing numeric vector assignment uses the same membership rules");
}
} // namespace

int main() {
    ParsingAndMembership();
    LargeRanges();
    NumericCallers();
    std::puts("PASS: compact mesh bake index filters");
}
