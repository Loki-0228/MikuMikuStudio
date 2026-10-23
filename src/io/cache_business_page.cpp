#include "cache_business_wire.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

// Byte mapping for one business index page. ValidatePage owns path spelling,
// record ordering and owner ordering; this file only maps wire fields. Every
// failure leaves the caller's output untouched.
namespace mikumikustudio::cache_business {
namespace {

using wire::Reader;
using wire::Writer;
struct Limit {};

inline constexpr std::uint32_t PageVersion = 1;
inline constexpr std::uint32_t PinnedFlag = 1;
inline constexpr std::size_t ProjectFixedBytes = 40;
inline constexpr std::size_t EntryFixedBytes = 192;
inline constexpr std::size_t CurrentRecordSize = 64;
inline constexpr std::size_t BlobReferenceBytes = 160;

static_assert(PageHeaderBytes == 64, "page header layout changed");
static_assert(ProjectFixedBytes == 40, "project record layout changed");
static_assert(EntryFixedBytes == 32 + BlobReferenceBytes, "entry record layout changed");
static_assert(CurrentRecordSize == 64, "current record layout changed");

std::size_t MinimumRecordBytes(PageKind kind) noexcept {
    switch (kind) {
    case PageKind::Projects:
        return ProjectFixedBytes;
    case PageKind::Entries:
        return EntryFixedBytes;
    case PageKind::Current:
        return CurrentRecordSize;
    }
    return 0;
}

std::size_t MaximumRecords(PageKind kind) noexcept {
    switch (kind) {
    case PageKind::Projects:
        return MaximumProjects;
    case PageKind::Entries:
        return MaximumEntries;
    case PageKind::Current:
        return MaximumCurrent;
    }
    return 0;
}

bool KnownKind(cache_files::Kind kind) noexcept {
    switch (kind) {
    case cache_files::Kind::Effect:
    case cache_files::Kind::Texture:
    case cache_files::Kind::CatalogSnapshot:
    case cache_files::Kind::CatalogCommit:
    case cache_files::Kind::CatalogCheckpoint:
    case cache_files::Kind::IndexPage:
        return true;
    }
    return false;
}

void PutProject(Writer& writer, const Project& project) {
    // recordBytes u32, flags u32, id u64, lastOpened u64, normalizedUnits u32,
    // displayUnits u32, zero u64, then both UTF-16LE arrays.
    const std::uint64_t recordBytes = detail::RecordBytes(project);
    const std::size_t start = writer.at;
    writer.Put(recordBytes, 4);
    writer.Put(project.pinned ? PinnedFlag : 0u, 4);
    writer.Put(project.id, 8);
    writer.Put(project.lastOpened, 8);
    writer.Put(static_cast<std::uint64_t>(project.normalizedPath.size()), 4);
    writer.Put(static_cast<std::uint64_t>(project.displayPath.size()), 4);
    writer.Zero(8);
    writer.String(project.normalizedPath);
    writer.String(project.displayPath);
    if (static_cast<std::uint64_t>(writer.at - start) != recordBytes) {
        throw wire::Invalid{};
    }
}

void PutEntry(Writer& writer, const Entry& entry) {
    // recordBytes u32, availability u32, consumerFormat u32, ownerCount u32,
    // lastUse u64, zero u64, 160-byte blob reference, then owner ids.
    const std::uint64_t recordBytes = detail::RecordBytes(entry);
    const std::size_t start = writer.at;
    writer.Put(recordBytes, 4);
    writer.Put(static_cast<std::uint32_t>(entry.availability), 4);
    writer.Put(entry.consumerFormat, 4);
    writer.Put(static_cast<std::uint64_t>(entry.owners.size()), 4);
    writer.Put(entry.lastUse, 8);
    writer.Zero(8);
    writer.Blob(entry.blob);
    for (const ProjectId owner : entry.owners) {
        writer.Put(owner, 8);
    }
    if (static_cast<std::uint64_t>(writer.at - start) != recordBytes) {
        throw wire::Invalid{};
    }
}

void PutCurrent(Writer& writer, const Current& current) {
    // recordBytes u32 = 64, kind u32, format u32, zero u32, key 32, generation 16.
    const std::size_t start = writer.at;
    writer.Put(static_cast<std::uint64_t>(CurrentRecordSize), 4);
    writer.Put(static_cast<std::uint32_t>(current.key.kind), 4);
    writer.Put(current.key.consumerFormat, 4);
    writer.Zero(4);
    writer.Array(current.key.key);
    writer.Array(current.generation);
    if (writer.at - start != CurrentRecordSize) {
        throw wire::Invalid{};
    }
}

void ParseProject(Reader& reader, Project& project) {
    const std::size_t start = reader.at;
    const std::uint64_t recordBytes = reader.Get(4);
    if (recordBytes < ProjectFixedBytes || recordBytes > reader.size - start) {
        throw wire::Invalid{};
    }
    const std::uint32_t flags = static_cast<std::uint32_t>(reader.Get(4));
    if ((flags & ~PinnedFlag) != 0) {
        throw wire::Invalid{};
    }
    project.pinned = (flags & PinnedFlag) != 0;
    project.id = reader.Get(8);
    project.lastOpened = reader.Get(8);
    const std::uint32_t normalizedUnits = static_cast<std::uint32_t>(reader.Get(4));
    const std::uint32_t displayUnits = static_cast<std::uint32_t>(reader.Get(4));
    reader.Zero(8);
    if (normalizedUnits > MaximumPathUnits || displayUnits > MaximumPathUnits) {
        throw Limit{};
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(ProjectFixedBytes) +
        2ull * (static_cast<std::uint64_t>(normalizedUnits) + displayUnits);
    if (expected != recordBytes) {
        throw wire::Invalid{};
    }
    project.normalizedPath = reader.String(normalizedUnits);
    project.displayPath = reader.String(displayUnits);
    if (static_cast<std::uint64_t>(reader.at - start) != recordBytes) {
        throw wire::Invalid{};
    }
}

void ParseEntry(Reader& reader, Entry& entry) {
    const std::size_t start = reader.at;
    const std::uint64_t recordBytes = reader.Get(4);
    if (recordBytes < EntryFixedBytes || recordBytes > reader.size - start) {
        throw wire::Invalid{};
    }
    const std::uint32_t availability = static_cast<std::uint32_t>(reader.Get(4));
    if (availability > static_cast<std::uint32_t>(Availability::Retired)) {
        throw wire::Invalid{};
    }
    entry.availability = static_cast<Availability>(availability);
    entry.consumerFormat = static_cast<std::uint32_t>(reader.Get(4));
    const std::uint32_t ownerCount = static_cast<std::uint32_t>(reader.Get(4));
    if (ownerCount > MaximumOwnersPerEntry) {
        throw Limit{};
    }
    entry.lastUse = reader.Get(8);
    reader.Zero(8);
    const std::uint64_t expected = static_cast<std::uint64_t>(EntryFixedBytes) +
        8ull * static_cast<std::uint64_t>(ownerCount);
    if (expected != recordBytes) {
        throw wire::Invalid{};
    }
    entry.blob = reader.Blob();
    if (!KnownKind(entry.blob.spec.kind)) {
        throw wire::Invalid{};
    }
    reader.Need(static_cast<std::size_t>(ownerCount) * 8);
    entry.owners.resize(ownerCount);
    for (ProjectId& owner : entry.owners) {
        owner = reader.Get(8);
    }
    if (static_cast<std::uint64_t>(reader.at - start) != recordBytes) {
        throw wire::Invalid{};
    }
}

void ParseCurrent(Reader& reader, Current& current) {
    const std::size_t start = reader.at;
    const std::uint64_t recordBytes = reader.Get(4);
    if (recordBytes != CurrentRecordSize || recordBytes > reader.size - start) {
        throw wire::Invalid{};
    }
    const std::uint32_t kind = static_cast<std::uint32_t>(reader.Get(4));
    if (!KnownKind(static_cast<cache_files::Kind>(kind))) {
        throw wire::Invalid{};
    }
    current.key.kind = static_cast<cache_files::Kind>(kind);
    current.key.consumerFormat = static_cast<std::uint32_t>(reader.Get(4));
    reader.Zero(4);
    reader.Array(current.key.key);
    reader.Array(current.generation);
    if (reader.at - start != CurrentRecordSize) {
        throw wire::Invalid{};
    }
}

void Publish(Page& source, Page& output) noexcept {
    using std::swap;
    swap(output.storeOwner, source.storeOwner);
    swap(output.kind, source.kind);
    swap(output.ordinal, source.ordinal);
    output.projects.swap(source.projects);
    output.entries.swap(source.entries);
    output.current.swap(source.current);
}

} // namespace

Result EncodePage(const Page& page, Bytes& output) noexcept {
    const Result validated = ValidatePage(page);
    if (validated != Result::Ok) {
        return validated;
    }
    try {
        // The writer is sized from the same record sizes the validator checked,
        // so a mismatch shows up as a short write or an overrun.
        const std::uint64_t total = detail::PageBytes(page);
        if (total > static_cast<std::uint64_t>(MaximumPageBytes)) {
            return Result::Limit;
        }
        if (total < PageHeaderBytes) {
            return Result::Invalid;
        }
        wire::Writer writer(static_cast<std::size_t>(total));
        writer.Magic(wire::PageMagic);
        writer.Put(PageVersion, 4);
        writer.Put(static_cast<std::uint32_t>(page.kind), 4);
        writer.Put(page.ordinal, 4);
        writer.Put(static_cast<std::uint32_t>(detail::Count(page)), 4);
        writer.Put(total - static_cast<std::uint64_t>(PageHeaderBytes), 8);
        writer.Array(page.storeOwner);
        writer.Zero(8);
        switch (page.kind) {
        case PageKind::Projects:
            for (const Project& project : page.projects) {
                PutProject(writer, project);
            }
            break;
        case PageKind::Entries:
            for (const Entry& entry : page.entries) {
                PutEntry(writer, entry);
            }
            break;
        case PageKind::Current:
            for (const Current& current : page.current) {
                PutCurrent(writer, current);
            }
            break;
        default:
            return Result::Invalid;
        }
        if (writer.at != writer.bytes.size()) {
            return Result::Invalid;
        }
        output.swap(writer.bytes);
        return Result::Ok;
    } catch (const wire::Invalid&) {
        return Result::Invalid;
    } catch (...) {
        return Result::AllocationFailure;
    }
}

Result DecodePage(const void* bytes, std::size_t size, Page& output) noexcept {
    if (bytes == nullptr || size < PageHeaderBytes) {
        return Result::Invalid;
    }
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    wire::Reader reader{data, size};
    try {
        reader.Magic(wire::PageMagic);
        const std::uint32_t version = static_cast<std::uint32_t>(reader.Get(4));
        if (version != PageVersion) {
            return Result::Unsupported;
        }
        if (size > MaximumPageBytes) {
            return Result::Limit;
        }
        const std::uint32_t rawKind = static_cast<std::uint32_t>(reader.Get(4));
        const std::uint32_t ordinal = static_cast<std::uint32_t>(reader.Get(4));
        const std::uint32_t recordCount = static_cast<std::uint32_t>(reader.Get(4));
        const std::uint64_t bodyBytes = reader.Get(8);
        cache_files::Owner owner{};
        reader.Array(owner);
        reader.Zero(8);
        if (rawKind < static_cast<std::uint32_t>(PageKind::Projects) ||
            rawKind > static_cast<std::uint32_t>(PageKind::Current)) {
            return Result::Invalid;
        }
        const PageKind kind = static_cast<PageKind>(rawKind);
        if (bodyBytes != static_cast<std::uint64_t>(size - PageHeaderBytes)) {
            return Result::Invalid;
        }
        // Bound every count before reserving storage: the declared body cannot
        // be shorter than recordCount minimum-sized records and each kind has a
        // hard record cap.
        if (recordCount == 0) return Result::Invalid;
        if (static_cast<std::size_t>(recordCount) > MaximumRecords(kind) || ordinal>=MaximumPages)
            return Result::Limit;
        const std::uint64_t minimumRecordBytes =
            static_cast<std::uint64_t>(MinimumRecordBytes(kind));
        if (static_cast<std::uint64_t>(recordCount) * minimumRecordBytes > bodyBytes) {
            return Result::Invalid;
        }
        Page parsed;
        parsed.storeOwner = owner;
        parsed.kind = kind;
        parsed.ordinal = ordinal;
        switch (kind) {
        case PageKind::Projects:
            parsed.projects.resize(recordCount);
            for (Project& project : parsed.projects) {
                ParseProject(reader, project);
            }
            break;
        case PageKind::Entries:
            parsed.entries.resize(recordCount);
            for (Entry& entry : parsed.entries) {
                ParseEntry(reader, entry);
            }
            break;
        case PageKind::Current:
            parsed.current.resize(recordCount);
            for (Current& current : parsed.current) {
                ParseCurrent(reader, current);
            }
            break;
        }
        if (reader.at != size) {
            return Result::Invalid;
        }
        const Result validated = ValidatePage(parsed);
        if (validated != Result::Ok) {
            return validated;
        }
        Publish(parsed, output);
        return Result::Ok;
    } catch (const Limit&) {
        return Result::Limit;
    } catch (const wire::Invalid&) {
        return Result::Invalid;
    } catch (...) {
        return Result::AllocationFailure;
    }
}

} // namespace mikumikustudio::cache_business
