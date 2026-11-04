// Standalone contract tests for the in-memory legacy MME host-import rewrite.
// Independent console main; no test framework and no NDEBUG-dependent asserts.
//
// Build example:
//   cl /nologo /std:c++17 /EHsc /utf-8 /I src\plugins ^
//      tests\plugins\legacy_image_tests.cpp src\plugins\legacy_image.cpp ^
//      /Fe:build\verification\legacy-image\legacy_image_tests.exe
#include "../../src/plugins/legacy_image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using mikumikustudio::plugins::LegacyImageError;
using mikumikustudio::plugins::LegacyImagePatch;
using mikumikustudio::plugins::RedirectLegacyHostImport;

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
}

void CheckLabeled(bool condition, const char* label, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s line %d: %s\n", label, line, expression);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)
#define CHECK_LABELED(label, expression) CheckLabeled((expression), (label), #expression, __LINE__)

// ---------------------------------------------------------------------------
// Minimal PE32+ DLL fixture. The image has two sections:
//   .rdata  RVA 0x1000  raw 0x400  size 0x800  (imports, names, thunks)
//   .text   RVA 0x2000  raw 0xC00  size 0x200
// Headers are 0x400 bytes and the file is 0xE00 bytes.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kPeOffset = 0x80;
constexpr std::uint32_t kDosLfanew = 0x3C;
constexpr std::uint32_t kOptionalOffset = kPeOffset + 24;
constexpr std::uint32_t kOptionalSize = 240;
constexpr std::uint32_t kSectionTableOffset = kOptionalOffset + kOptionalSize;
constexpr std::uint32_t kSection0 = kSectionTableOffset;
constexpr std::uint32_t kSection1 = kSectionTableOffset + 40;
constexpr std::uint32_t kSizeOfHeaders = 0x400;
constexpr std::uint32_t kSectionAlignment = 0x1000;
constexpr std::uint32_t kFileAlignment = 0x200;
constexpr std::uint32_t kRdataRva = 0x1000;
constexpr std::uint32_t kRdataRaw = 0x400;
constexpr std::uint32_t kRdataSize = 0x800;
constexpr std::uint32_t kTextRva = 0x2000;
constexpr std::uint32_t kTextRaw = 0xC00;
constexpr std::uint32_t kTextSize = 0x200;
constexpr std::uint32_t kImageSize = 0x3000;
constexpr std::uint32_t kFileSize = kTextRaw + kTextSize;
constexpr std::uint32_t kDescriptorRaw = kRdataRaw;
constexpr std::uint32_t kDescriptorRva = kRdataRva;
constexpr std::uint32_t kNameRaw = kRdataRaw + 0x200;
constexpr std::uint32_t kNameRva = kRdataRva + 0x200;
constexpr std::uint32_t kThunkRva = kRdataRva + 0x500;
constexpr std::uint32_t kExtraRva = kRdataRva + 0x700;

constexpr std::uint32_t kImportDirectoryOffset = kOptionalOffset + 112 + 1 * 8;
constexpr std::uint32_t kBoundDirectoryOffset = kOptionalOffset + 112 + 11 * 8;
constexpr std::uint32_t kDelayDirectoryOffset = kOptionalOffset + 112 + 13 * 8;
constexpr std::uint32_t kOptionalMagicOffset = kOptionalOffset;
constexpr std::uint32_t kOptionalSectionAlignmentOffset = kOptionalOffset + 32;
constexpr std::uint32_t kOptionalSizeOfHeadersOffset = kOptionalOffset + 60;
constexpr std::uint32_t kMachineOffset = kPeOffset + 4;
constexpr std::uint32_t kNumberOfSectionsOffset = kPeOffset + 6;
constexpr std::uint32_t kSizeOfOptionalHeaderOffset = kPeOffset + 20;
constexpr std::uint32_t kCharacteristicsOffset = kPeOffset + 22;

constexpr std::uint32_t kSectionVirtualSize = 8;
constexpr std::uint32_t kSectionVirtualAddress = 12;
constexpr std::uint32_t kSectionRawSize = 16;
constexpr std::uint32_t kSectionRawOffset = 20;

constexpr std::uint32_t kDescriptorOriginalFirstThunk = 0;
constexpr std::uint32_t kDescriptorTimeDateStamp = 4;
constexpr std::uint32_t kDescriptorForwarderChain = 8;
constexpr std::uint32_t kDescriptorName = 12;
constexpr std::uint32_t kDescriptorFirstThunk = 16;

constexpr char kAdapterText[] = "MMSAdapter.dll";
constexpr std::size_t kAdapterLength = sizeof(kAdapterText) - 1;

void Put16(std::vector<std::uint8_t>& image, std::size_t offset, std::uint16_t value) {
    image[offset] = static_cast<std::uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void Put32(std::vector<std::uint8_t>& image, std::size_t offset, std::uint32_t value) {
    image[offset] = static_cast<std::uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    image[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    image[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void Put64(std::vector<std::uint8_t>& image, std::size_t offset, std::uint64_t value) {
    Put32(image, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
    Put32(image, offset + 4, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
}

struct Fixture {
    std::vector<std::uint8_t> image;
    std::vector<std::uint32_t> name_offsets;
    std::vector<std::uint32_t> name_slots;
    std::uint32_t descriptor_count = 0;
};

Fixture MakeFixture(std::initializer_list<std::string_view> imports) {
    Fixture fixture;
    fixture.image.assign(kFileSize, 0);
    std::vector<std::uint8_t>& image = fixture.image;
    const std::size_t importCount = imports.size();
    fixture.descriptor_count = static_cast<std::uint32_t>(importCount);

    Put16(image, 0, 0x5A4D);                                  // IMAGE_DOS_SIGNATURE
    Put32(image, kDosLfanew, kPeOffset);                      // e_lfanew
    Put32(image, kPeOffset, 0x00004550);                      // IMAGE_NT_SIGNATURE
    Put16(image, kMachineOffset, 0x8664);                     // IMAGE_FILE_MACHINE_AMD64
    Put16(image, kNumberOfSectionsOffset, 2);
    Put16(image, kSizeOfOptionalHeaderOffset, static_cast<std::uint16_t>(kOptionalSize));
    Put16(image, kCharacteristicsOffset, 0x2022);             // EXECUTABLE | LARGE_ADDRESS_AWARE | DLL

    Put16(image, kOptionalMagicOffset, 0x20B);                // PE32+
    Put32(image, kOptionalOffset + 4, kTextSize);             // SizeOfCode
    Put32(image, kOptionalOffset + 8, kRdataSize);            // SizeOfInitializedData
    Put32(image, kOptionalOffset + 16, kTextRva);             // AddressOfEntryPoint
    Put32(image, kOptionalOffset + 20, kTextRva);             // BaseOfCode
    Put64(image, kOptionalOffset + 24, 0x140000000ull);       // ImageBase
    Put32(image, kOptionalSectionAlignmentOffset, kSectionAlignment);
    Put32(image, kOptionalOffset + 36, kFileAlignment);
    Put16(image, kOptionalOffset + 40, 6);                    // MajorOperatingSystemVersion
    Put16(image, kOptionalOffset + 42, 1);                    // MinorOperatingSystemVersion
    Put16(image, kOptionalOffset + 48, 6);                    // MajorSubsystemVersion
    Put16(image, kOptionalOffset + 50, 1);                    // MinorSubsystemVersion
    Put32(image, kOptionalOffset + 56, kImageSize);           // SizeOfImage
    Put32(image, kOptionalSizeOfHeadersOffset, kSizeOfHeaders);
    Put16(image, kOptionalOffset + 68, 2);                    // IMAGE_SUBSYSTEM_WINDOWS_GUI
    Put64(image, kOptionalOffset + 72, 0x100000);             // SizeOfStackReserve
    Put64(image, kOptionalOffset + 80, 0x1000);               // SizeOfStackCommit
    Put64(image, kOptionalOffset + 88, 0x100000);             // SizeOfHeapReserve
    Put64(image, kOptionalOffset + 96, 0x1000);               // SizeOfHeapCommit
    Put32(image, kOptionalOffset + 108, 16);                  // NumberOfRvaAndSizes

    const std::uint32_t importDirectorySize = static_cast<std::uint32_t>((importCount + 1) * 20);
    Put32(image, kImportDirectoryOffset, kDescriptorRva);
    Put32(image, kImportDirectoryOffset + 4, importDirectorySize);

    auto writeSection = [&image](std::size_t offset, const char* name, std::uint32_t virtualSize,
                                 std::uint32_t virtualAddress, std::uint32_t rawSize,
                                 std::uint32_t rawOffset, std::uint32_t characteristics) {
        std::memcpy(image.data() + offset, name, std::strlen(name));
        Put32(image, offset + kSectionVirtualSize, virtualSize);
        Put32(image, offset + kSectionVirtualAddress, virtualAddress);
        Put32(image, offset + kSectionRawSize, rawSize);
        Put32(image, offset + kSectionRawOffset, rawOffset);
        Put32(image, offset + 36, characteristics);
    };
    writeSection(kSection0, ".rdata", kRdataSize, kRdataRva, kRdataSize, kRdataRaw, 0x40000040);
    writeSection(kSection1, ".text", kTextSize, kTextRva, kTextSize, kTextRaw, 0x60000020);

    std::uint32_t nameCursor = kNameRaw;
    for (std::size_t index = 0; index < importCount; ++index) {
        const std::size_t descriptor = kDescriptorRaw + index * 20;
        const std::uint32_t thunkRva = kThunkRva + static_cast<std::uint32_t>(index) * 8;
        const std::uint32_t nameRva = kNameRva + (nameCursor - kNameRaw);
        Put32(image, descriptor + kDescriptorOriginalFirstThunk, thunkRva);
        Put32(image, descriptor + kDescriptorTimeDateStamp, 0);
        Put32(image, descriptor + kDescriptorForwarderChain, 0);
        Put32(image, descriptor + kDescriptorName, nameRva);
        Put32(image, descriptor + kDescriptorFirstThunk, thunkRva);
        const std::string_view name = *(imports.begin() + static_cast<std::ptrdiff_t>(index));
        std::memcpy(image.data() + nameCursor, name.data(), name.size());
        image[nameCursor + name.size()] = 0;
        fixture.name_offsets.push_back(nameCursor);
        fixture.name_slots.push_back(static_cast<std::uint32_t>(name.size() + 1));
        nameCursor += static_cast<std::uint32_t>(name.size() + 1);
    }
    return fixture;
}

std::vector<std::size_t> ChangedIndices(const std::vector<std::uint8_t>& before,
                                        const std::vector<std::uint8_t>& after) {
    std::vector<std::size_t> changed;
    const std::size_t common = std::min(before.size(), after.size());
    for (std::size_t index = 0; index < common; ++index) {
        if (before[index] != after[index]) changed.push_back(index);
    }
    return changed;
}

bool InsideAny(const std::vector<std::pair<std::size_t, std::size_t>>& ranges, std::size_t index) {
    for (const auto& range : ranges) {
        if (index >= range.first && index < range.first + range.second) return true;
    }
    return false;
}

void ExpectFailure(const std::vector<std::uint8_t>& image, LegacyImageError expected, const char* label) {
    const std::vector<std::uint8_t> before = image;
    const LegacyImagePatch result = RedirectLegacyHostImport(image);
    const bool rejected = !result.ok() && result.error == expected && result.image.empty() &&
                          result.rewritten == 0 && !result.message.empty() && image == before;
    CHECK_LABELED(label, rejected);
    if (!rejected) {
        std::fprintf(stderr, "  expected %s, got %s, message=\"%s\"\n",
                     mikumikustudio::plugins::LegacyImageErrorName(expected),
                     mikumikustudio::plugins::LegacyImageErrorName(result.error),
                     result.message.c_str());
    }
}

bool Rejected(const std::vector<std::uint8_t>& image) {
    const LegacyImagePatch result = RedirectLegacyHostImport(image);
    return !result.ok() && result.image.empty() && result.rewritten == 0 && !result.message.empty();
}

void CheckSuccess(const Fixture& fixture, std::initializer_list<std::size_t> matched, const char* label) {
    const std::vector<std::uint8_t> before = fixture.image;
    LegacyImagePatch result = RedirectLegacyHostImport(fixture.image);
    CHECK_LABELED(label, result.ok());
    CHECK_LABELED(label, result.error == LegacyImageError::None);
    CHECK_LABELED(label, result.message.empty());
    CHECK_LABELED(label, result.rewritten == matched.size());
    CHECK_LABELED(label, result.image.size() == fixture.image.size());
    CHECK_LABELED(label, fixture.image == before);
    if (!result.ok() || result.image.size() != fixture.image.size()) return;
    CHECK_LABELED(label, result.image.data() != fixture.image.data());

    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const std::size_t index : matched) {
        const std::size_t offset = fixture.name_offsets[index];
        const std::size_t slot = fixture.name_slots[index];
        ranges.emplace_back(offset, slot);
        for (std::size_t position = 0; position < kAdapterLength; ++position) {
            CHECK_LABELED(label, result.image[offset + position] == static_cast<std::uint8_t>(kAdapterText[position]));
        }
        for (std::size_t position = kAdapterLength; position < slot; ++position) {
            CHECK_LABELED(label, result.image[offset + position] == 0);
        }
    }

    const std::vector<std::size_t> changed = ChangedIndices(before, result.image);
    CHECK_LABELED(label, !changed.empty());
    for (std::size_t index : changed) {
        CHECK_LABELED(label, InsideAny(ranges, index));
    }

    result.image[0] = static_cast<std::uint8_t>(result.image[0] ^ 0xFF);
    CHECK_LABELED(label, fixture.image == before);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // A matched import is rewritten in place; every other byte is preserved.
    {
        const Fixture fixture = MakeFixture({"KERNEL32.dll", "MikuMikuDance.exe", "d3d9.dll"});
        CheckSuccess(fixture, {1}, "basic success");
    }

    // The adapted copy is accepted as input but no longer contains the legacy
    // name, so a second pass must fail instead of reporting success again.
    {
        const Fixture fixture = MakeFixture({"KERNEL32.dll", "MikuMikuDance.exe"});
        const LegacyImagePatch adapted = RedirectLegacyHostImport(fixture.image);
        CHECK(adapted.ok());
        CHECK(adapted.rewritten == 1);
        ExpectFailure(adapted.image, LegacyImageError::NoMatchingImport, "already adapted");
    }

    // The comparison is ASCII case-insensitive.
    {
        const char* variants[] = {"mikumikudance.exe", "MIKUMIKUDANCE.EXE", "mIkUmIkUdAnCe.ExE"};
        for (const char* variant : variants) {
            const Fixture fixture = MakeFixture({"KERNEL32.dll", variant});
            CheckSuccess(fixture, {1}, variant);
        }
    }

    // Every matching descriptor is rewritten and counted.
    {
        const Fixture fixture =
            MakeFixture({"MikuMikuDance.exe", "KERNEL32.dll", "mikumikudance.exe", "USER32.dll"});
        CheckSuccess(fixture, {0, 2}, "multiple matches");
    }

    // Absent or unmatched imports fail with an empty result.
    {
        const Fixture onlySystem = MakeFixture({"KERNEL32.dll", "USER32.dll"});
        ExpectFailure(onlySystem.image, LegacyImageError::NoMatchingImport, "no matching name");
        const Fixture emptyTable = MakeFixture({});
        ExpectFailure(emptyTable.image, LegacyImageError::NoMatchingImport, "empty import table");
        Fixture noDirectory = MakeFixture({"KERNEL32.dll"});
        Put32(noDirectory.image, kImportDirectoryOffset, 0);
        Put32(noDirectory.image, kImportDirectoryOffset + 4, 0);
        ExpectFailure(noDirectory.image, LegacyImageError::NoMatchingImport, "no import directory");
    }

    // DOS and PE signatures.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put16(bad.image, 0, 0x1234);
        ExpectFailure(bad.image, LegacyImageError::BadDosHeader, "bad DOS signature");
        bad = fixture;
        Put32(bad.image, kPeOffset, 0x12345678);
        ExpectFailure(bad.image, LegacyImageError::BadPeSignature, "bad PE signature");
        bad = fixture;
        Put32(bad.image, kDosLfanew, 0);
        ExpectFailure(bad.image, LegacyImageError::BadDosHeader, "zero e_lfanew");
        bad = fixture;
        Put32(bad.image, kDosLfanew, 0xFFFFFFF0u);
        ExpectFailure(bad.image, LegacyImageError::BadDosHeader, "negative e_lfanew");
        bad = fixture;
        Put32(bad.image, kDosLfanew, 0x7FFFFFF0u);
        ExpectFailure(bad.image, LegacyImageError::Truncated, "e_lfanew beyond image");
    }

    // Truncation: every strict prefix of a valid image is rejected, and no
    // failure returns bytes.
    {
        const Fixture fixture = MakeFixture({"KERNEL32.dll", "MikuMikuDance.exe"});
        ExpectFailure(std::vector<std::uint8_t>{}, LegacyImageError::EmptyInput, "empty input");
        const std::vector<std::uint8_t> tiny(fixture.image.begin(), fixture.image.begin() + 10);
        ExpectFailure(tiny, LegacyImageError::Truncated, "tiny input");
        std::size_t firstAccepted = fixture.image.size();
        for (std::size_t length = 0; length < fixture.image.size(); ++length) {
            const std::vector<std::uint8_t> prefix(fixture.image.begin(),
                fixture.image.begin() + static_cast<std::ptrdiff_t>(length));
            if (!Rejected(prefix)) {
                firstAccepted = length;
                break;
            }
        }
        CHECK_LABELED("truncation prefixes", firstAccepted == fixture.image.size());
    }

    // Optional header shape.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put16(bad.image, kOptionalMagicOffset, 0x10B);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedOptionalHeader, "PE32 optional header");
        bad = fixture;
        Put32(bad.image, kOptionalSectionAlignmentOffset, 0x123);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedOptionalHeader, "unaligned sections");
        bad = fixture;
        Put16(bad.image, kSizeOfOptionalHeaderOffset, 128);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedOptionalHeader, "truncated data directories");
        bad = fixture;
        Put32(bad.image, kOptionalSizeOfHeadersOffset, 0x100000u);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedOptionalHeader, "headers beyond image");
    }

    // Machine, DLL flag, and section count.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put16(bad.image, kMachineOffset, 0x014C);
        ExpectFailure(bad.image, LegacyImageError::NotAmd64, "i386 machine");
        bad = fixture;
        Put16(bad.image, kCharacteristicsOffset, 0x0022);
        ExpectFailure(bad.image, LegacyImageError::NotADll, "executable image");
        bad = fixture;
        Put16(bad.image, kNumberOfSectionsOffset, 0);
        ExpectFailure(bad.image, LegacyImageError::InvalidSectionTable, "zero sections");
        bad = fixture;
        Put16(bad.image, kNumberOfSectionsOffset, 0xFFFF);
        ExpectFailure(bad.image, LegacyImageError::InvalidSectionTable, "absurd section count");
    }

    // Arithmetic overflow and out-of-image ranges.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put32(bad.image, kSection0 + kSectionRawOffset, 0xFFFFFFF0u);
        Put32(bad.image, kSection0 + kSectionRawSize, 0x200u);
        ExpectFailure(bad.image, LegacyImageError::Overflow, "raw range overflow");
        bad = fixture;
        Put32(bad.image, kSection0 + kSectionVirtualAddress, 0xFFFFFFF0u);
        Put32(bad.image, kSection0 + kSectionVirtualSize, 0x200u);
        ExpectFailure(bad.image, LegacyImageError::Overflow, "virtual range overflow");
        bad = fixture;
        Put32(bad.image, kSection0 + kSectionVirtualAddress, 0x4000u);
        ExpectFailure(bad.image, LegacyImageError::InvalidSectionTable, "virtual range beyond image");
        bad = fixture;
        Put32(bad.image, kSection1 + kSectionRawSize, 0x400u);
        ExpectFailure(bad.image, LegacyImageError::Truncated, "raw range beyond file");
        bad = fixture;
        Put32(bad.image, kSection0 + kSectionRawOffset, 0x300u);
        ExpectFailure(bad.image, LegacyImageError::InvalidSectionTable, "raw data inside headers");
    }

    // Overlapping and ambiguous section mappings.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put32(bad.image, kSection1 + kSectionVirtualAddress, 0x1400u);
        ExpectFailure(bad.image, LegacyImageError::SectionOverlap, "virtual section overlap");
        bad = fixture;
        Put32(bad.image, kSection1 + kSectionRawOffset, 0x500u);
        ExpectFailure(bad.image, LegacyImageError::SectionOverlap, "raw section overlap");
        bad = fixture;
        Put32(bad.image, kSection0 + kSectionVirtualAddress, 0x300u);
        ExpectFailure(bad.image, LegacyImageError::AmbiguousRvaMapping, "section overlapping headers");
    }

    // Import directory ranges and sizes.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put32(bad.image, kImportDirectoryOffset, 0x9000u);
        ExpectFailure(bad.image, LegacyImageError::RvaOutOfRange, "import directory RVA");
        bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, 0);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDirectory, "import size zero");
        bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, 10);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDirectory, "import size too small");
        bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, 21);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDirectory, "import size not descriptor aligned");
        bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, 0x1000014u);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDirectory, "import size above ceiling");
        bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, 0x80Cu);
        ExpectFailure(bad.image, LegacyImageError::RvaOutOfRange, "import table crosses section");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorName, 0x9000u);
        ExpectFailure(bad.image, LegacyImageError::RvaOutOfRange, "import name RVA");
    }

    // Import table and descriptor forms.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put32(bad.image, kImportDirectoryOffset + 4, fixture.descriptor_count * 20);
        ExpectFailure(bad.image, LegacyImageError::ImportTableUnterminated, "missing terminator");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorName, 0);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDescriptor, "descriptor without name");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorName, kThunkRva);
        ExpectFailure(bad.image, LegacyImageError::InvalidImportDescriptor, "empty import name");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorName, kRdataRva + kRdataSize - 1);
        bad.image[kRdataRaw + kRdataSize - 1] = 'A';
        ExpectFailure(bad.image, LegacyImageError::UnterminatedImportName, "unterminated import name");
    }

    // Bound and delay imports are unsupported in this version.
    {
        const Fixture fixture = MakeFixture({"MikuMikuDance.exe"});
        Fixture bad = fixture;
        Put32(bad.image, kBoundDirectoryOffset, kExtraRva);
        Put32(bad.image, kBoundDirectoryOffset + 4, 8);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedBoundImports, "bound import directory");
        bad = fixture;
        Put32(bad.image, kDelayDirectoryOffset, kExtraRva);
        Put32(bad.image, kDelayDirectoryOffset + 4, 32);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedDelayImports, "delay import directory");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorTimeDateStamp, 1);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedBoundImports, "descriptor timestamp");
        bad = fixture;
        Put32(bad.image, kDescriptorRaw + kDescriptorForwarderChain, 1);
        ExpectFailure(bad.image, LegacyImageError::UnsupportedBoundImports, "descriptor forwarder chain");
    }

    // The published names are the ones the caller expects.
    CHECK(std::string(mikumikustudio::plugins::LegacyHostImportName) == "MikuMikuDance.exe");
    CHECK(std::string(mikumikustudio::plugins::LegacyAdapterModuleName) == "MMSAdapter.dll");
    CHECK(std::string(mikumikustudio::plugins::LegacyImageErrorName(LegacyImageError::None)) == "None");

    std::printf("legacy image rewrite %s checks=%u failures=%u\n",
                g_failures == 0 ? "PASSED" : "FAILED", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
