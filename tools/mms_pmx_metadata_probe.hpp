#pragma once

#include "mms_format_probe.hpp"

#include <string>

namespace mikumikustudio::format_probe {

enum class PmxMetadataStatus { Absent, Unsupported, Truncated, Invalid, MetadataOk, IoError, BudgetExceeded };

inline constexpr std::array<std::string_view, 4> kPmxMetadataFields{
    "name_local", "name_en", "comment_local", "comment_en"};
inline constexpr std::size_t kMaxPmxFieldBytes = 64 * 1024;
inline constexpr std::size_t kMaxPmxTextBytes = 256 * 1024;

struct PmxMetadataResult {
    Result header{};
    PmxMetadataStatus status = PmxMetadataStatus::Absent;
    std::size_t bytesExamined = 0;
    std::array<std::u32string, kPmxMetadataFields.size()> fields{};
};

namespace pmx_metadata_detail {

inline constexpr std::size_t kHeaderSize = 17;

inline std::uint32_t LittleEndian(const char* data, std::size_t size) {
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < size; ++i)
        n |= std::uint32_t{static_cast<unsigned char>(data[i])} << (i * 8);
    return n;
}

inline bool DecodeUtf8(std::string_view bytes, std::u32string& out) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[i++]);
        if (lead < 0x80) {
            out.push_back(lead);
            continue;
        }
        char32_t value, minimum;
        unsigned tail;
        if (lead >= 0xC2 && lead <= 0xDF) {
            value = lead & 0x1F;
            tail = 1;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            value = lead & 0x0F;
            tail = 2;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            value = lead & 0x07;
            tail = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (tail > bytes.size() - i) return false;
        for (; tail; --tail) {
            const auto next = static_cast<unsigned char>(bytes[i++]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return false;
        out.push_back(value);
    }
    return true;
}

inline bool DecodeUtf16Le(std::string_view bytes, std::u32string& out) {
    if (bytes.size() % 2) return false;
    for (std::size_t i = 0; i < bytes.size();) {
        auto value = LittleEndian(bytes.data() + i, 2);
        i += 2;
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (bytes.size() - i < 2) return false;
            const auto low = LittleEndian(bytes.data() + i, 2);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            i += 2;
            value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
        } else if (value >= 0xDC00 && value <= 0xDFFF) {
            return false;
        }
        out.push_back(static_cast<char32_t>(value));
    }
    return true;
}

}  // namespace pmx_metadata_detail

// Use the existing native-header interpretation for reporting, but gate text
// preview on the standard layout emitted by tools/pmxlib.py. Read only the
// length and contents of each field; do not sample bytes beyond the fourth.
template<class ReadAt>
PmxMetadataResult InspectPmxMetadata(std::uint64_t fileSize, ReadAt readAt) {
    using namespace pmx_metadata_detail;
    PmxMetadataResult out;
    const auto read = [&](std::uint64_t offset, char* data, std::size_t count) {
        if (!readAt(offset, data, count)) {
            out.status = PmxMetadataStatus::IoError;
            return false;
        }
        out.bytesExamined += count;
        return true;
    };

    std::array<char, kPrefixSize> prefix{};
    auto prefixSize = static_cast<std::size_t>((std::min)(fileSize, std::uint64_t{kHeaderSize}));
    if (prefixSize && !read(0, prefix.data(), prefixSize)) {
        out.header.status = Status::IoError;
        return out;
    }
    if (!StartsWith({prefix.data(), prefixSize}, kPmxMagic)) {
        // Identify other headers without probing any PMM footer in this mode.
        const auto fullSize = static_cast<std::size_t>((std::min)(fileSize, std::uint64_t{kPrefixSize}));
        if (fullSize > prefixSize &&
            !read(prefixSize, prefix.data() + prefixSize, fullSize - prefixSize)) {
            out.header.status = Status::IoError;
            return out;
        }
        prefixSize = fullSize;
    }
    out.header = ProbePrefix({prefix.data(), prefixSize});
    if (out.header.format != Format::Pmx) return out;
    if (out.header.status != Status::Recognized) {
        out.status = PmxMetadataStatus::Truncated;
        return out;
    }
    if (prefix[3] != ' ') {
        out.status = PmxMetadataStatus::Invalid;
        return out;
    }
    if (out.header.version == "other" || out.header.encoding == "other" ||
        static_cast<unsigned char>(prefix[8]) > 8) {
        out.status = PmxMetadataStatus::Unsupported;
        return out;
    }
    if (static_cast<unsigned char>(prefix[8]) != 8 ||
        static_cast<unsigned char>(prefix[10]) > 4 ||
        !std::all_of(prefix.begin() + 11, prefix.begin() + kHeaderSize,
                     [](char width) { return width == 1 || width == 2 || width == 4; })) {
        out.status = PmxMetadataStatus::Invalid;
        return out;
    }

    const bool utf16 = out.header.encoding == "utf-16le";
    auto offset = std::uint64_t{kHeaderSize};
    std::size_t textBytes = 0;
    std::array<std::u32string, kPmxMetadataFields.size()> fields;
    for (auto& field : fields) {
        std::array<char, 4> length{};
        if (length.size() > fileSize - offset) {
            out.status = PmxMetadataStatus::Truncated;
            return out;
        }
        if (!read(offset, length.data(), length.size())) return out;
        offset += length.size();
        const auto count = LittleEndian(length.data(), length.size());
        // PMX lengths are signed little-endian byte counts, even for UTF-16LE.
        if ((count & 0x80000000u) || (utf16 && count % 2)) {
            out.status = PmxMetadataStatus::Invalid;
            return out;
        }
        if (count > kMaxPmxFieldBytes || count > kMaxPmxTextBytes - textBytes) {
            out.status = PmxMetadataStatus::BudgetExceeded;
            return out;
        }
        if (count > fileSize - offset) {
            out.status = PmxMetadataStatus::Truncated;
            return out;
        }
        if (!count) continue;
        std::string bytes(count, '\0');
        if (!read(offset, bytes.data(), bytes.size())) return out;
        offset += count;
        textBytes += count;
        field.reserve(utf16 ? count / 2 : count);
        if (!(utf16 ? DecodeUtf16Le(bytes, field) : DecodeUtf8(bytes, field))) {
            out.status = PmxMetadataStatus::Invalid;
            return out;
        }
    }
    out.fields.swap(fields);
    out.status = PmxMetadataStatus::MetadataOk;
    return out;
}

}  // namespace mikumikustudio::format_probe
