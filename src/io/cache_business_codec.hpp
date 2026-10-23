#pragma once
#include "mikumikustudio/cache_files.hpp"

// Internal business index encoding. No I/O, budget grants or recovery authority.
namespace mikumikustudio::cache_business {
using ProjectId = std::uint64_t;
// Decode the 32 lowercase filename hex digits left-to-right, two per byte;
// this is not the in-memory byte order of a Windows GUID structure.
using GenerationId = std::array<std::uint8_t, 16>;
using Bytes = std::vector<std::uint8_t>;
enum class Availability : std::uint32_t { Valid=0, Invalid=1, Retired=2 };
enum class PageKind : std::uint32_t { Projects=1, Entries=2, Current=3 };
enum class Result { Ok, Invalid, Unsupported, Limit, AllocationFailure };
inline constexpr std::size_t MaximumProjects=10000, MaximumEntries=100000,
    MaximumCurrent=100000, MaximumOwnersPerEntry=10000, MaximumPathUnits=32768;
inline constexpr std::size_t MaximumPageBytes=1024*1024, MaximumPages=128;
inline constexpr std::uint64_t MaximumLiveBytes=64ull*1024*1024;
inline constexpr std::size_t PageHeaderBytes=64, ManifestHeaderBytes=96, PageRefBytes=176;
inline constexpr cache_files::Digest PageKey{4};
struct LogicalKey {
    cache_files::Kind kind=cache_files::Kind::Effect;
    std::uint32_t consumerFormat=0;
    cache_files::Digest key{};
};
struct Project {
    ProjectId id=0;
    std::u16string normalizedPath,displayPath;
    std::uint64_t lastOpened=0;
    bool pinned=false;
};
struct Entry {
    cache_files::BlobInfo blob;
    std::uint32_t consumerFormat=0;
    std::uint64_t lastUse=0;
    Availability availability=Availability::Valid;
    std::vector<ProjectId> owners;
};
struct Current { LogicalKey key; GenerationId generation{}; };
struct Index {
    cache_files::Owner storeOwner{};
    std::uint64_t maintenanceEpoch=0,nextProjectId=0;
    std::vector<Project> projects;
    std::vector<Entry> entries;
    std::vector<Current> current;
};
struct Page {
    cache_files::Owner storeOwner{};
    PageKind kind=PageKind::Projects;
    std::uint32_t ordinal=0;
    std::vector<Project> projects;
    std::vector<Entry> entries;
    std::vector<Current> current;
};
struct PageRef {
    PageKind kind=PageKind::Projects;
    std::uint32_t ordinal=0,recordCount=0;
    cache_files::BlobInfo blob;
};
struct Manifest {
    cache_files::Owner storeOwner{};
    std::uint64_t maintenanceEpoch=0,nextProjectId=0;
    std::uint32_t projectCount=0,entryCount=0,currentCount=0;
    std::uint64_t liveEncodedBytes=0;
    std::vector<PageRef> pages;
};
// All errors preserve outputs. No operation normalizes, truncates or drops data.
// Projects are id-ordered; entries by (kind,name); current by logical key.
// Path spelling is preserved, but project identity comparison is Win7 ordinal
// case-insensitive. Both paths must be lexical absolute DOS/UNC, no dot segments,
// device prefixes, ADS, slash separators, trailing dot/space or reserved names.
// normalizedPath and displayPath must name the same ordinal-insensitive path;
// aliases/junctions/short names are not resolved by this codec.
// Unpaired UTF-16 surrogate code units are preserved.
Result Validate(const Index&) noexcept;
Result ValidatePage(const Page&) noexcept;
Result ValidateManifest(const Manifest&) noexcept;
Result Partition(const Index&,std::vector<Page>& output) noexcept;
Result EncodePage(const Page&,Bytes& output) noexcept;
Result DecodePage(const void*,std::size_t,Page& output) noexcept;
Result EncodeManifest(const Manifest&,Bytes& output) noexcept;
Result DecodeManifest(const void*,std::size_t,Manifest& output) noexcept;
// Pages must appear in manifest order. The caller must Acquire/ReadAll each
// PageRef first: decoded values alone cannot prove file identity or digest.
Result Assemble(const Manifest&,const std::vector<Page>&,Index& output) noexcept;
namespace detail {
bool ValidOwner(const cache_files::Owner&) noexcept;
bool ValidBlob(const cache_files::BlobInfo&,const cache_files::Owner&,bool page) noexcept;
std::uint64_t RecordBytes(const Project&) noexcept;
std::uint64_t RecordBytes(const Entry&) noexcept;
std::uint64_t PageBytes(const Page&) noexcept;
std::size_t Count(const Page&) noexcept;
bool KeyLess(const LogicalKey&,const LogicalKey&) noexcept;
}
}
