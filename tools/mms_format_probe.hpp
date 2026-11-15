#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Internal to mms_format_inspect. No SDK or native loader dependencies.
namespace mikumikustudio::format_probe {

enum class Status { Recognized, Incomplete, Unrecognized, IoError };
enum class Format { Pmm, Pmx, Vmd, Unknown };
enum class Gate { Accepted, Rejected, Unknown };
enum class Footer { NotChecked, NotDetected, MarkerOnly };

struct Result {
    Status status = Status::Unrecognized;
    Format format = Format::Unknown;
    std::string_view version{};
    std::string_view encoding{};
    Gate full = Gate::Unknown;
    Gate original = Gate::Unknown;
    Footer pmm3end = Footer::NotChecked;
    std::size_t bytesExamined = 0;
};

inline constexpr std::size_t kPrefixSize = 30;
inline constexpr std::size_t kFooterSize = 64;
inline constexpr std::string_view kPmmMagic = "Polygon Movie maker";
inline constexpr std::string_view kPmxMagic = "PMX";
inline constexpr std::string_view kVmdMagic = "Vocaloid Motion Data ";
inline constexpr std::string_view kPmm3End{"PMM3END\0", 8};

inline bool StartsWith(std::string_view bytes, std::string_view prefix) {
    return bytes.substr(0, prefix.size()) == prefix;
}

inline Result ProbePrefix(std::string_view bytes) {
    Result out;
    out.bytesExamined = bytes.size();
    const auto mark = [&](Format format, bool complete) {
        out.format = format;
        out.status = complete ? Status::Recognized : Status::Incomplete;
    };
    const auto setGate = [&](bool accepted) {
        out.full = out.original = accepted ? Gate::Accepted : Gate::Rejected;
    };

    if (StartsWith(bytes, kPmmMagic)) {
        // pmm_load_shell.cpp: first 19 bytes, then version + NUL at byte
        // 20 of a 30-byte header. Byte 19 and final padding are ignored.
        mark(Format::Pmm, bytes.size() >= kPrefixSize);
        if (out.status == Status::Incomplete) return out;
        const auto version = bytes.substr(20, 5);
        if (version == std::string_view{"0001\0", 5}) out.version = "0001";
        else if (version == std::string_view{"0002\0", 5}) out.version = "0002";
        else if (version == std::string_view{"0003\0", 5}) out.version = "0003";
        else out.version = "other";
        setGate(out.version != "other");
        if (out.version == "0003") out.original = Gate::Rejected;
    } else if (StartsWith(bytes, kPmxMagic)) {
        // pmd_load.cpp dispatches on "PMX". pmx_load.cpp then consumes an
        // ignored byte, a LE float, a settings-length byte (not checked by
        // the loader), and eight settings, the first of which is encoding.
        mark(Format::Pmx, bytes.size() >= 17);
        if (out.status == Status::Incomplete) return out;
        const auto version = bytes.substr(4, 4);
        if (version == std::string_view{"\x00\x00\x00\x40", 4}) out.version = "2.0";
        else if (version == std::string_view{"\x66\x66\x06\x40", 4}) out.version = "2.1";
        else out.version = "other";
        out.encoding = bytes[9] == 0 ? "utf-16le" : bytes[9] == 1 ? "utf-8" : "other";
        setGate(out.version == "2.0" && out.encoding == "utf-16le");
    } else if (StartsWith(bytes, kVmdMagic)) {
        // vmd_read_guard.hpp: strcmp on a 30-byte field, which requires
        // the NUL after either version tag but ignores bytes after it.
        mark(Format::Vmd, bytes.size() >= kPrefixSize);
        if (out.status == Status::Incomplete) return out;
        const auto version = bytes.substr(kVmdMagic.size(), 5);
        if (version == std::string_view{"file\0", 5}) out.version = "1";
        else if (version == std::string_view{"0002\0", 5}) out.version = "2";
        else out.version = "other";
        setGate(out.version != "other");
    } else if (!bytes.empty() &&
               (StartsWith(kPmmMagic, bytes) || StartsWith(kPmxMagic, bytes) ||
                StartsWith(kVmdMagic, bytes))) {
        out.status = Status::Incomplete;
    }
    return out;
}

// ReadAt must fill exactly the requested bytes at an absolute offset without
// read-ahead. The second window never overlaps the first. Only a recognized
// PMM 0003 wrapper can receive the optional standalone codec marker check.
template<class ReadAt>
Result Inspect(std::uint64_t fileSize, ReadAt readAt) {
    std::array<char, kPrefixSize> prefix{};
    const auto prefixSize =
        static_cast<std::size_t>((std::min)(fileSize, std::uint64_t{kPrefixSize}));
    if (prefixSize && !readAt(0, prefix.data(), prefixSize)) return {Status::IoError};
    auto out = ProbePrefix({prefix.data(), prefixSize});
    if (out.format == Format::Pmm && out.version == "0003" &&
        fileSize >= kPrefixSize + kFooterSize) {
        std::array<char, kFooterSize> footer{};
        if (!readAt(fileSize - kFooterSize, footer.data(), footer.size())) return {Status::IoError};
        out.bytesExamined += footer.size();
        // pmm_container.cpp compares exactly these eight bytes at EOF-64.
        // No sizes, CRCs, table, blocks, or native PMM tails are verified.
        out.pmm3end = StartsWith({footer.data(), footer.size()}, kPmm3End)
            ? Footer::MarkerOnly : Footer::NotDetected;
    }
    return out;
}

}  // namespace mikumikustudio::format_probe
