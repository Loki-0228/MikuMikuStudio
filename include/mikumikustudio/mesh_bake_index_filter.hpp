#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace mikumikustudio::alembic_export {

// A blank filter selects all indices. A nonblank filter stores sorted,
// disjoint inclusive ranges. Adjacent ranges are merged as well.
class IndexFilter {
public:
    IndexFilter() = default;
    // Numeric callers can still supply a list or vector of individual indices.
    IndexFilter(std::initializer_list<std::uint32_t> indices) { SetIndices(indices); }
    IndexFilter(const std::vector<std::uint32_t>& indices) { SetIndices(indices); }

    bool empty() const noexcept { return ranges_.empty(); }
    std::size_t RangeCount() const noexcept { return ranges_.size(); }

    bool Allows(std::uint32_t index) const noexcept {
        if (empty()) return true;
        const auto range = std::lower_bound(ranges_.begin(), ranges_.end(), index,
            [](const Range& candidate, std::uint32_t value) { return candidate.last < value; });
        return range != ranges_.end() && range->first <= index;
    }

    // Keep the dialog's comma-separated syntax and upper bound. Do not
    // change the output on invalid input.
    static bool Parse(const std::wstring& text, IndexFilter& out) {
        constexpr unsigned long maximum = 16000000;
        IndexFilter parsed;
        const wchar_t* p = text.c_str();
        while (*p) {
            while (std::iswspace(*p) || *p == L',') ++p;
            if (!*p) break;
            if (!std::iswdigit(*p)) return false;
            wchar_t* end = nullptr;
            const auto first = std::wcstoul(p, &end, 10);
            if (end == p) return false;
            p = end;
            while (std::iswspace(*p)) ++p;
            auto last = first;
            if (*p == L'-') {
                ++p;
                while (std::iswspace(*p)) ++p;
                if (!std::iswdigit(*p)) return false;
                last = std::wcstoul(p, &end, 10);
                if (end == p) return false;
                p = end;
            }
            if (last < first || last > maximum) return false;
            parsed.ranges_.push_back({std::uint32_t(first), std::uint32_t(last)});
            while (std::iswspace(*p)) ++p;
            if (*p && *p != L',') return false;
        }
        parsed.MergeRanges();
        out = std::move(parsed);
        return true;
    }

private:
    struct Range { std::uint32_t first, last; };
    std::vector<Range> ranges_;

    template<class Indices>
    void SetIndices(const Indices& indices) {
        ranges_.reserve(indices.size());
        for (const auto index : indices) ranges_.push_back({index, index});
        MergeRanges();
    }

    void MergeRanges() {
        std::sort(ranges_.begin(), ranges_.end(),
            [](const Range& a, const Range& b) { return a.first < b.first; });
        std::size_t merged = 0;
        for (const auto range : ranges_) {
            if (merged) {
                auto& previous = ranges_[merged - 1];
                // Short-circuit overlapping ranges before subtracting; the
                // adjacent case stays safe even with numeric UINT32_MAX input.
                if (range.first <= previous.last || range.first - previous.last == 1) {
                    if (range.last > previous.last) previous.last = range.last;
                    continue;
                }
            }
            ranges_[merged++] = range;
        }
        ranges_.resize(merged);
    }
};

} // namespace mikumikustudio::alembic_export
