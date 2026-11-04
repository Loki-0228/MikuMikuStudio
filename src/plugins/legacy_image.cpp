#include "legacy_image.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof(IMAGE_FILE_HEADER) == 20, "PE file header layout");
static_assert(sizeof(IMAGE_DATA_DIRECTORY) == 8, "PE data directory layout");
static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "PE section header layout");
static_assert(sizeof(IMAGE_IMPORT_DESCRIPTOR) == 20, "PE import descriptor layout");
static_assert(offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) == 112, "PE32+ data directory offset");

namespace mikumikustudio::plugins {
namespace {

constexpr char kHostImport[] = "MikuMikuDance.exe";
constexpr char kAdapterImport[] = "MMSAdapter.dll";
constexpr std::size_t kHostImportLength = sizeof(kHostImport) - 1;
constexpr std::size_t kAdapterImportLength = sizeof(kAdapterImport) - 1;
constexpr std::uint32_t kMaxSections = 96;
constexpr std::uint32_t kDataDirectoryCount = 16;
constexpr std::uint32_t kMaxImportDirectorySize = 4096u * 20u;
constexpr std::uint32_t kMinFileAlignment = 0x200;
constexpr std::uint32_t kMaxFileAlignment = 0x10000;
constexpr std::size_t kOptionalHeaderDirectoryOffset = 112; // Pinned by static_assert above.
constexpr std::uint64_t kMaximumRva = 0xFFFFFFFFull;

struct Directory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct Section {
    std::uint32_t virtualAddress = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t rawOffset = 0;
    std::uint32_t rawSize = 0;
};

struct Patch {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

bool IsPowerOfTwo(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool Overlaps(std::uint32_t firstOffset, std::uint32_t firstSize,
              std::uint32_t secondOffset, std::uint32_t secondSize) noexcept {
    if (firstSize == 0 || secondSize == 0) return false;
    return std::uint64_t(firstOffset) < std::uint64_t(secondOffset) + secondSize &&
           std::uint64_t(secondOffset) < std::uint64_t(firstOffset) + firstSize;
}

bool EqualsHostImportIgnoreCase(const std::uint8_t* text, std::size_t length) noexcept {
    if (length != kHostImportLength) return false;
    for (std::size_t index = 0; index < length; ++index) {
        std::uint8_t value = text[index];
        if (value >= 'A' && value <= 'Z') value = static_cast<std::uint8_t>(value + ('a' - 'A'));
        auto expected = static_cast<std::uint8_t>(kHostImport[index]);
        if (expected >= 'A' && expected <= 'Z') expected = static_cast<std::uint8_t>(expected + ('a' - 'A'));
        if (value != expected) return false;
    }
    return true;
}

class PeImage final {
public:
    explicit PeImage(const std::vector<std::uint8_t>& image) noexcept : image_(image) {}

    bool Analyze() {
        const std::uint64_t total = image_.size();
        if (total == 0) return Fail(LegacyImageError::EmptyInput, "image is empty");
        if (total > kMaximumRva) return Fail(LegacyImageError::Overflow, "image exceeds 4 GiB");
        if (total < sizeof(IMAGE_DOS_HEADER)) return Fail(LegacyImageError::Truncated, "DOS header");
        if (U16(0) != IMAGE_DOS_SIGNATURE) return Fail(LegacyImageError::BadDosHeader, "DOS signature");
        const std::int32_t dosLfanew = I32(offsetof(IMAGE_DOS_HEADER, e_lfanew));
        if (dosLfanew < static_cast<std::int32_t>(sizeof(IMAGE_DOS_HEADER))) {
            return Fail(LegacyImageError::BadDosHeader, "e_lfanew");
        }
        const std::uint32_t peOffset = static_cast<std::uint32_t>(dosLfanew);
        if (!Has(peOffset, sizeof(std::uint32_t) + sizeof(IMAGE_FILE_HEADER))) {
            return Fail(LegacyImageError::Truncated, "PE header");
        }
        if (U32(peOffset) != IMAGE_NT_SIGNATURE) {
            return Fail(LegacyImageError::BadPeSignature, "PE signature");
        }
        const std::size_t fileHeader = static_cast<std::size_t>(peOffset) + sizeof(std::uint32_t);
        const std::uint16_t machine = U16(fileHeader + offsetof(IMAGE_FILE_HEADER, Machine));
        if (machine != IMAGE_FILE_MACHINE_AMD64) {
            return Fail(LegacyImageError::NotAmd64, "expected AMD64");
        }
        const std::uint16_t characteristics = U16(fileHeader + offsetof(IMAGE_FILE_HEADER, Characteristics));
        if ((characteristics & IMAGE_FILE_DLL) == 0) {
            return Fail(LegacyImageError::NotADll, "expected a DLL");
        }
        numberOfSections_ = U16(fileHeader + offsetof(IMAGE_FILE_HEADER, NumberOfSections));
        if (numberOfSections_ == 0 || numberOfSections_ > kMaxSections) {
            return Fail(LegacyImageError::InvalidSectionTable, "section count");
        }
        const std::uint16_t optionalSize = U16(fileHeader + offsetof(IMAGE_FILE_HEADER, SizeOfOptionalHeader));
        const std::size_t optionalHeader = fileHeader + sizeof(IMAGE_FILE_HEADER);
        if (optionalSize < kOptionalHeaderDirectoryOffset + 2 * sizeof(IMAGE_DATA_DIRECTORY)) {
            return Fail(LegacyImageError::UnsupportedOptionalHeader, "optional header too small for imports");
        }
        if (!Has(optionalHeader, optionalSize)) {
            return Fail(LegacyImageError::Truncated, "optional header");
        }
        if (U16(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, Magic)) != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return Fail(LegacyImageError::UnsupportedOptionalHeader, "expected PE32+");
        }
        const std::uint32_t sectionAlignment = U32(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, SectionAlignment));
        const std::uint32_t fileAlignment = U32(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, FileAlignment));
        if (!IsPowerOfTwo(sectionAlignment) || !IsPowerOfTwo(fileAlignment) ||
            fileAlignment < kMinFileAlignment || fileAlignment > kMaxFileAlignment ||
            sectionAlignment < fileAlignment) {
            return Fail(LegacyImageError::UnsupportedOptionalHeader, "invalid section/file alignment");
        }
        sizeOfImage_ = U32(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage));
        sizeOfHeaders_ = U32(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfHeaders));
        if (sizeOfImage_ == 0 || sizeOfHeaders_ == 0 || std::uint64_t(sizeOfHeaders_) > total) {
            return Fail(LegacyImageError::UnsupportedOptionalHeader, "invalid image/header size");
        }
        const std::uint32_t declaredDirectories =
            U32(optionalHeader + offsetof(IMAGE_OPTIONAL_HEADER64, NumberOfRvaAndSizes));
        const std::size_t directoryCapacity = (optionalSize - kOptionalHeaderDirectoryOffset) / sizeof(IMAGE_DATA_DIRECTORY);
        if (declaredDirectories > directoryCapacity) {
            return Fail(LegacyImageError::UnsupportedOptionalHeader, "data directory array");
        }
        directoryCount_ = static_cast<std::size_t>(
            declaredDirectories < kDataDirectoryCount ? declaredDirectories : kDataDirectoryCount);
        optionalHeaderOffset_ = optionalHeader;
        sectionTableOffset_ = optionalHeader + optionalSize;
        const std::uint64_t sectionTableBytes =
            std::uint64_t(numberOfSections_) * sizeof(IMAGE_SECTION_HEADER);
        if (!Has(sectionTableOffset_, sectionTableBytes)) {
            return Fail(LegacyImageError::Truncated, "section table");
        }
        if (std::uint64_t(sectionTableOffset_) + sectionTableBytes > sizeOfHeaders_) {
            return Fail(LegacyImageError::InvalidSectionTable, "section table outside headers");
        }

        sections_.reserve(numberOfSections_);
        for (std::size_t index = 0; index < numberOfSections_; ++index) {
            const std::size_t header = sectionTableOffset_ + index * sizeof(IMAGE_SECTION_HEADER);
            Section section;
            section.virtualSize = U32(header + offsetof(IMAGE_SECTION_HEADER, Misc));
            section.virtualAddress = U32(header + offsetof(IMAGE_SECTION_HEADER, VirtualAddress));
            section.rawSize = U32(header + offsetof(IMAGE_SECTION_HEADER, SizeOfRawData));
            section.rawOffset = U32(header + offsetof(IMAGE_SECTION_HEADER, PointerToRawData));
            if (section.rawSize != 0) {
                const std::uint64_t rawEnd = std::uint64_t(section.rawOffset) + section.rawSize;
                if (rawEnd > kMaximumRva) return Fail(LegacyImageError::Overflow, "section raw range");
                if (!Has(section.rawOffset, section.rawSize)) {
                    return Fail(LegacyImageError::Truncated, "section raw range");
                }
                if (section.rawOffset < sizeOfHeaders_) {
                    return Fail(LegacyImageError::InvalidSectionTable, "section raw data inside headers");
                }
            }
            if (section.virtualSize != 0) {
                const std::uint64_t virtualEnd = std::uint64_t(section.virtualAddress) + section.virtualSize;
                if (virtualEnd > kMaximumRva) return Fail(LegacyImageError::Overflow, "section virtual range");
                if (virtualEnd > sizeOfImage_) {
                    return Fail(LegacyImageError::InvalidSectionTable, "section virtual range");
                }
                if (section.virtualAddress < sizeOfHeaders_) {
                    return Fail(LegacyImageError::AmbiguousRvaMapping, "section overlaps headers");
                }
            }
            sections_.push_back(section);
        }
        for (std::size_t first = 0; first < sections_.size(); ++first) {
            for (std::size_t second = first + 1; second < sections_.size(); ++second) {
                if (Overlaps(sections_[first].virtualAddress, sections_[first].virtualSize,
                             sections_[second].virtualAddress, sections_[second].virtualSize) ||
                    Overlaps(sections_[first].rawOffset, sections_[first].rawSize,
                             sections_[second].rawOffset, sections_[second].rawSize)) {
                    return Fail(LegacyImageError::SectionOverlap, "overlapping sections");
                }
            }
        }
        return true;
    }

    bool CollectPatches(std::vector<Patch>& patches) {
        const Directory bound = DirectoryAt(static_cast<std::size_t>(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT));
        if (bound.rva != 0 || bound.size != 0) {
            return Fail(LegacyImageError::UnsupportedBoundImports, "bound import directory");
        }
        const Directory delay = DirectoryAt(static_cast<std::size_t>(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT));
        if (delay.rva != 0 || delay.size != 0) {
            return Fail(LegacyImageError::UnsupportedDelayImports, "delay import directory");
        }
        const Directory imports = DirectoryAt(static_cast<std::size_t>(IMAGE_DIRECTORY_ENTRY_IMPORT));
        if (imports.rva == 0 && imports.size == 0) {
            return Fail(LegacyImageError::NoMatchingImport, "no import directory");
        }
        if (imports.rva == 0 || imports.size == 0) {
            return Fail(LegacyImageError::InvalidImportDirectory, "incomplete import directory");
        }
        if (imports.size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            return Fail(LegacyImageError::InvalidImportDirectory, "import directory too small");
        }
        if (imports.size % sizeof(IMAGE_IMPORT_DESCRIPTOR) != 0) {
            return Fail(LegacyImageError::InvalidImportDirectory, "import directory size");
        }
        if (imports.size > kMaxImportDirectorySize) {
            return Fail(LegacyImageError::InvalidImportDirectory, "import directory too large");
        }
        std::uint32_t directoryOffset = 0;
        std::uint32_t directoryLimit = 0;
        if (!Map(imports.rva, imports.size, directoryOffset, directoryLimit)) {
            return Fail(LegacyImageError::RvaOutOfRange, "import directory RVA");
        }
        (void)directoryLimit;

        const std::size_t descriptorCount = imports.size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        bool terminated = false;
        for (std::size_t index = 0; index < descriptorCount; ++index) {
            const std::size_t descriptor =
                static_cast<std::size_t>(directoryOffset) + index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            const std::uint32_t originalFirstThunk =
                U32(descriptor + offsetof(IMAGE_IMPORT_DESCRIPTOR, OriginalFirstThunk));
            const std::uint32_t timeDateStamp = U32(descriptor + offsetof(IMAGE_IMPORT_DESCRIPTOR, TimeDateStamp));
            const std::uint32_t forwarderChain = U32(descriptor + offsetof(IMAGE_IMPORT_DESCRIPTOR, ForwarderChain));
            const std::uint32_t name = U32(descriptor + offsetof(IMAGE_IMPORT_DESCRIPTOR, Name));
            const std::uint32_t firstThunk = U32(descriptor + offsetof(IMAGE_IMPORT_DESCRIPTOR, FirstThunk));
            if (originalFirstThunk == 0 && timeDateStamp == 0 && forwarderChain == 0 &&
                name == 0 && firstThunk == 0) {
                terminated = true;
                break;
            }
            if (name == 0) {
                return Fail(LegacyImageError::InvalidImportDescriptor, "import descriptor without module name");
            }
            if (timeDateStamp != 0 || forwarderChain != 0) {
                return Fail(LegacyImageError::UnsupportedBoundImports, "bound import descriptor");
            }
            std::uint32_t nameOffset = 0;
            std::uint32_t nameLimit = 0;
            if (!Map(name, 1, nameOffset, nameLimit)) {
                return Fail(LegacyImageError::RvaOutOfRange, "import module name");
            }
            std::uint32_t end = nameOffset;
            const auto boundedLimit = static_cast<std::uint32_t>(std::min<std::uint64_t>(nameLimit, std::uint64_t(nameOffset) + 260));
            while (end < boundedLimit && Byte(end) != 0) ++end;
            if (end == boundedLimit) {
                return Fail(LegacyImageError::UnterminatedImportName, "import module name");
            }
            const std::size_t length = static_cast<std::size_t>(end) - nameOffset;
            if (length == 0) {
                return Fail(LegacyImageError::InvalidImportDescriptor, "empty import module name");
            }
            std::uint32_t completeOffset = 0, completeLimit = 0;
            if (!Map(name, static_cast<std::uint32_t>(length + 1), completeOffset, completeLimit))
                return Fail(LegacyImageError::RvaOutOfRange, "complete import module name");
            if (!EqualsHostImportIgnoreCase(image_.data() + nameOffset, length)) continue;
            const std::size_t slot = length + 1;
            if (slot > std::numeric_limits<std::uint32_t>::max()) {
                return Fail(LegacyImageError::Overflow, "import name slot");
            }
            if (kAdapterImportLength + 1 > slot) {
                return Fail(LegacyImageError::ImportNameSlotTooSmall, "replacement name does not fit");
            }
            if (!Has(nameOffset, slot)) {
                return Fail(LegacyImageError::Truncated, "import name slot");
            }
            Patch patch;
            patch.offset = nameOffset;
            patch.size = static_cast<std::uint32_t>(slot);
            patches.push_back(patch);
        }
        if (!terminated) {
            return Fail(LegacyImageError::ImportTableUnterminated, "import directory has no terminator");
        }
        if (patches.empty()) {
            return Fail(LegacyImageError::NoMatchingImport, "no matching import");
        }
        return true;
    }

    LegacyImageError error() const noexcept { return error_; }
    const std::string& message() const noexcept { return message_; }

private:
    bool Fail(LegacyImageError error, const char* detail) {
        error_ = error;
        message_.assign(LegacyImageErrorName(error));
        if (detail != nullptr && detail[0] != '\0') {
            message_.append(": ");
            message_.append(detail);
        }
        return false;
    }

    bool Has(std::uint64_t offset, std::uint64_t size) const noexcept {
        return offset <= image_.size() && size <= image_.size() - offset;
    }

    std::uint8_t Byte(std::size_t offset) const noexcept { return image_[offset]; }

    std::uint16_t U16(std::size_t offset) const noexcept {
        return static_cast<std::uint16_t>(image_[offset]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(image_[offset + 1]) << 8);
    }

    std::uint32_t U32(std::size_t offset) const noexcept {
        return static_cast<std::uint32_t>(image_[offset]) |
               (static_cast<std::uint32_t>(image_[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(image_[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(image_[offset + 3]) << 24);
    }

    std::int32_t I32(std::size_t offset) const noexcept {
        return static_cast<std::int32_t>(U32(offset));
    }

    Directory DirectoryAt(std::size_t index) const noexcept {
        Directory directory;
        if (index >= directoryCount_) return directory;
        const std::size_t offset = optionalHeaderOffset_ + kOptionalHeaderDirectoryOffset +
                                   index * sizeof(IMAGE_DATA_DIRECTORY);
        directory.rva = U32(offset);
        directory.size = U32(offset + offsetof(IMAGE_DATA_DIRECTORY, Size));
        return directory;
    }

    bool Map(std::uint32_t rva, std::uint32_t size,
             std::uint32_t& fileOffset, std::uint32_t& fileLimit) const noexcept {
        const std::uint64_t end = std::uint64_t(rva) + size;
        if (end > kMaximumRva) return false;
        if (rva < sizeOfHeaders_) {
            if (end > sizeOfHeaders_ || end > image_.size()) return false;
            fileOffset = rva;
            fileLimit = sizeOfHeaders_;
            return true;
        }
        for (const Section& section : sections_) {
            if (section.virtualSize == 0 || rva < section.virtualAddress) continue;
            const std::uint64_t delta = std::uint64_t(rva) - section.virtualAddress;
            if (delta >= section.virtualSize) continue;
            if (delta + size > section.virtualSize || delta + size > section.rawSize) return false;
            fileOffset = section.rawOffset + static_cast<std::uint32_t>(delta);
            fileLimit = section.rawOffset + section.rawSize;
            return true;
        }
        return false;
    }

    const std::vector<std::uint8_t>& image_;
    std::vector<Section> sections_;
    std::size_t optionalHeaderOffset_ = 0;
    std::size_t sectionTableOffset_ = 0;
    std::size_t directoryCount_ = 0;
    std::uint32_t numberOfSections_ = 0;
    std::uint32_t sizeOfImage_ = 0;
    std::uint32_t sizeOfHeaders_ = 0;
    LegacyImageError error_ = LegacyImageError::None;
    std::string message_;
};

} // namespace

const char* LegacyImageErrorName(LegacyImageError error) noexcept {
    switch (error) {
    case LegacyImageError::None: return "None";
    case LegacyImageError::EmptyInput: return "EmptyInput";
    case LegacyImageError::Truncated: return "Truncated";
    case LegacyImageError::BadDosHeader: return "BadDosHeader";
    case LegacyImageError::BadPeSignature: return "BadPeSignature";
    case LegacyImageError::UnsupportedOptionalHeader: return "UnsupportedOptionalHeader";
    case LegacyImageError::NotAmd64: return "NotAmd64";
    case LegacyImageError::NotADll: return "NotADll";
    case LegacyImageError::InvalidSectionTable: return "InvalidSectionTable";
    case LegacyImageError::SectionOverlap: return "SectionOverlap";
    case LegacyImageError::AmbiguousRvaMapping: return "AmbiguousRvaMapping";
    case LegacyImageError::UnsupportedBoundImports: return "UnsupportedBoundImports";
    case LegacyImageError::UnsupportedDelayImports: return "UnsupportedDelayImports";
    case LegacyImageError::InvalidImportDirectory: return "InvalidImportDirectory";
    case LegacyImageError::ImportTableUnterminated: return "ImportTableUnterminated";
    case LegacyImageError::InvalidImportDescriptor: return "InvalidImportDescriptor";
    case LegacyImageError::RvaOutOfRange: return "RvaOutOfRange";
    case LegacyImageError::UnterminatedImportName: return "UnterminatedImportName";
    case LegacyImageError::ImportNameSlotTooSmall: return "ImportNameSlotTooSmall";
    case LegacyImageError::NoMatchingImport: return "NoMatchingImport";
    case LegacyImageError::Overflow: return "Overflow";
    case LegacyImageError::AllocationFailed: return "AllocationFailed";
    }
    return "Unknown";
}

LegacyImagePatch RedirectLegacyHostImport(const std::vector<std::uint8_t>& image) noexcept {
    LegacyImagePatch result;
    try {
        PeImage parsed(image);
        if (!parsed.Analyze()) {
            result.error = parsed.error();
            result.message = parsed.message();
            return result;
        }
        std::vector<Patch> patches;
        if (!parsed.CollectPatches(patches)) {
            result.error = parsed.error();
            result.message = parsed.message();
            return result;
        }
        std::vector<std::uint8_t> output = image;
        for (const Patch& patch : patches) {
            const std::size_t offset = patch.offset;
            std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), patch.size, std::uint8_t{0});
            std::memcpy(output.data() + offset, kAdapterImport, kAdapterImportLength);
        }
        result.image = std::move(output);
        result.rewritten = patches.size();
    } catch (const std::bad_alloc&) {
        result = LegacyImagePatch{};
        result.error = LegacyImageError::AllocationFailed;
        // Do not allocate again while reporting allocator exhaustion.
    }
    return result;
}

} // namespace mikumikustudio::plugins
