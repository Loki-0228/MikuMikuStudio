#pragma once

#include <Windows.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mikumikustudio::project_relocation {

enum class Status { NoMatch, Candidate, Ambiguous, InvalidPath, TooLong };
struct Result {
    Status status;
    std::wstring path;
};

namespace detail {
// Capacities are in UTF-16 units, including the terminating NUL.
inline constexpr size_t PathCapacity = 32768;

inline bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && (a.empty() ||
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                             b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}

inline bool ValidName(std::wstring_view name) {
    if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L"<>:\"|?*") != name.npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; })) return false;
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    for (auto device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (Equal(stem, device)) return false;
    if (stem.size() != 4) return true;
    const auto port = stem.substr(0, 3);
    const bool numbered = std::wstring_view(L"123456789\u00b9\u00b2\u00b3").find(stem[3]) != stem.npos;
    return !(numbered && (Equal(port, L"COM") || Equal(port, L"LPT")));
}

struct FilePath {
    std::wstring volume; // "D:" or "\\server\share" (the share is indivisible)
    std::vector<std::wstring> parts;

    std::wstring Prefix(size_t count) const {
        auto path = volume;
        for (size_t i = 0; i < count; ++i) {
            path += L'\\';
            path += parts[i];
        }
        return path;
    }
};

// Consume already decoded absolute file references. In particular, neither
// the current directory nor a model picker's last directory is an anchor.
inline bool Parse(std::wstring_view input, FilePath& file) {
    if (input.size() < 3 || input.size() >= PathCapacity) return false;
    std::wstring text(input);
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.back() == L'\\') return false;
    const std::wstring_view path(text);
    size_t start;
    if (path.substr(0, 2) == L"\\\\") {
        const auto serverEnd = path.find(L'\\', 2);
        if (serverEnd == path.npos || !ValidName(path.substr(2, serverEnd - 2))) return false;
        const auto shareEnd = path.find(L'\\', serverEnd + 1);
        if (shareEnd == path.npos) return false;
        const auto share = path.substr(serverEnd + 1, shareEnd - serverEnd - 1);
        if (!ValidName(share) || Equal(share, L"pipe") || Equal(share, L"mailslot") ||
            Equal(share, L"IPC$")) return false;
        file.volume = path.substr(0, shareEnd);
        start = shareEnd + 1;
    } else {
        const auto drive = path.front();
        if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) ||
            path.substr(1, 2) != L":\\") return false;
        file.volume = path.substr(0, 2);
        if (drive >= L'a' && drive <= L'z') file.volume[0] = drive - L'a' + L'A';
        start = 3;
    }
    while (start < path.size()) {
        const auto end = path.find(L'\\', start);
        const auto part = path.substr(start, end - start);
        if (part == L".." || part == L".") {
            if (end == path.npos) return false; // denotes a directory
            if (part == L"..") {
                if (file.parts.empty()) return false;
                file.parts.pop_back();
            }
        } else {
            if (!ValidName(part)) return false;
            file.parts.emplace_back(part);
        }
        if (end == path.npos) break;
        start = end + 1;
    }
    return !file.parts.empty();
}
} // namespace detail

inline std::wstring NormalizeFile(std::wstring_view absolute) {
    detail::FilePath file;
    return detail::Parse(absolute, file) ? file.Prefix(file.parts.size()) : std::wstring{};
}

// Owned by one recovery context. Callers supply only final manual successes
// and query only still-missing references. A candidate still needs the real
// loader's capacity, file identity and format checks before acceptance.
class RootMappings {
    struct Rule {
        std::wstring oldPrefix, newPrefix; // includes final separator
        size_t volumeLength;
    };
    struct FileSelection {
        std::wstring original, replacement;
    };
    std::vector<Rule> rules_;
    std::vector<FileSelection> selections_;
public:
    // False means the exact selection supplies no reusable directory mapping.
    bool AddConfirmed(std::wstring_view original, std::wstring_view loaded) {
        detail::FilePath from, to;
        if (!detail::Parse(original, from) || !detail::Parse(loaded, to) ||
            !detail::Equal(from.parts.back(), to.parts.back())) return false;
        size_t a = from.parts.size() - 1, b = to.parts.size() - 1;
        if (!a || !b) return false;
        // This boundary also covers later stream records: substituting here
        // preserves the same suffix as substituting at their common parent.
        // Retain one named directory on each side of the move.
        while (a > 1 && b > 1 && from.parts[a - 1] == to.parts[b - 1]) {
            --a;
            --b;
        }
        Rule rule{from.Prefix(a) + L'\\', to.Prefix(b) + L'\\', from.volume.size()};
        if (detail::Equal(from.volume, to.volume) &&
            std::wstring_view(rule.oldPrefix).substr(rule.volumeLength) ==
                std::wstring_view(rule.newPrefix).substr(to.volume.size())) return false;
        for (const auto& known : rules_)
            if (rule.oldPrefix == known.oldPrefix &&
                rule.newPrefix == known.newPrefix) return true;
        rules_.push_back(std::move(rule));
        return true;
    }

    bool AddConfirmedFile(std::wstring_view original, std::wstring_view loaded) {
        auto from = NormalizeFile(original);
        auto to = NormalizeFile(loaded);
        if (from.empty() || to.empty() || from == to) return false;
        AddConfirmed(from, to);
        const auto existing = std::find_if(selections_.begin(), selections_.end(),
            [&](const auto& selection) { return selection.original == from; });
        if (existing == selections_.end()) selections_.push_back({std::move(from), std::move(to)});
        else existing->replacement = std::move(to);
        return true;
    }

    Result Infer(std::wstring_view pending, size_t capacity = detail::PathCapacity) const {
        detail::FilePath file;
        if (pending.size() >= detail::PathCapacity) return {Status::TooLong, {}};
        if (!detail::Parse(pending, file)) return {Status::InvalidPath, {}};
        const auto path = file.Prefix(file.parts.size());
        // Repeated references use the exact confirmed file even if later
        // selections add conflicting or more specific directory rules.
        for (const auto& selection : selections_) {
            if (selection.original != path) continue;
            if (selection.replacement.size() >= (std::min)(capacity, detail::PathCapacity))
                return {Status::TooLong, {}};
            return {Status::Candidate, selection.replacement};
        }
        const Rule* best = nullptr;
        bool ambiguous = false;
        bool caseOnly = false;
        for (const auto& rule : rules_) {
            const auto length = rule.oldPrefix.size();
            const auto prefix = std::wstring_view(path).substr(0, length);
            if (!detail::Equal(prefix, rule.oldPrefix)) continue;
            // The old directory is missing, so its case semantics are unknown.
            // Keep drive and UNC volume matching, but don't alias directory names.
            if (prefix.substr(rule.volumeLength) !=
                std::wstring_view(rule.oldPrefix).substr(rule.volumeLength)) {
                caseOnly = true;
                continue;
            }
            if (!best || length > best->oldPrefix.size()) {
                best = &rule;
                ambiguous = false;
            } else if (length == best->oldPrefix.size() && rule.newPrefix != best->newPrefix) {
                // Different destination spellings could name distinct files
                // (including case-sensitive directories). Do not guess.
                ambiguous = true;
            }
        }
        if (!best) return {caseOnly ? Status::Ambiguous : Status::NoMatch, {}};
        if (ambiguous) return {Status::Ambiguous, {}};
        const auto suffix = std::wstring_view(path).substr(best->oldPrefix.size());
        if (best->newPrefix.size() + suffix.size() >= (std::min)(capacity, detail::PathCapacity))
            return {Status::TooLong, {}};
        auto candidate = best->newPrefix;
        candidate.append(suffix);
        return {Status::Candidate, std::move(candidate)};
    }
};
} // namespace mikumikustudio::project_relocation
