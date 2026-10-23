#include "mikumikustudio/cache_catalog.hpp"
#include "cache_catalog_codec.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace mikumikustudio::cache_catalog {
namespace {
namespace files = cache_files;
bool Equal(const files::BlobInfo& a, const files::BlobInfo& b) noexcept {
    return a.name == b.name && a.spec.owner == b.spec.owner && a.spec.kind == b.spec.kind &&
        a.spec.key == b.spec.key && a.payloadBytes == b.payloadBytes && a.payloadDigest == b.payloadDigest &&
        a.identity.volume == b.identity.volume && a.identity.fileIndex == b.identity.fileIndex &&
        a.identity.fileBytes == b.identity.fileBytes;
}
Result Convert(files::Result value) noexcept {
    switch (value) {
    case files::Result::Ok: return Result::Ok;
    case files::Result::Missing: return Result::Missing;
    case files::Result::Invalid: return Result::Invalid;
    case files::Result::Busy: return Result::Busy;
    case files::Result::Limit: return Result::Limit;
    case files::Result::Io: return Result::Io;
    default: return Result::RecoveryRequired;
    }
}
Result StoredError(files::Result value) noexcept {
    if (value == files::Result::Missing || value == files::Result::Invalid || value == files::Result::Limit)
        return Result::RecoveryRequired;
    return Convert(value);
}
struct Loaded {
    std::uint64_t revision = 0, metadataBytes = 0;
    std::size_t snapshots = 0, commits = 0;
    files::Owner owner{};
    files::BlobInfo head;
    std::vector<std::uint8_t> bytes;
    std::vector<files::ReadLease> leases;
};
struct Node { codec::Record record; files::BlobInfo head; };

// Every history record must remain valid until an independently validated
// compaction protocol exists. A damaged latest record never selects an old head.
Result Load(const files::Directory& snapshots, const files::Directory& commits, Loaded& output) {
    std::vector<std::wstring> commitNames, snapshotNames;
    auto io = commits.EnumerateAll(MaximumCommits, commitNames);
    if (io != files::Result::Ok) return Convert(io);
    io = snapshots.EnumerateAll(MaximumSnapshots, snapshotNames);
    if (io != files::Result::Ok) return Convert(io);
    if (commitNames.empty()) return snapshotNames.empty() ? Result::Missing : Result::RecoveryRequired;
    Loaded candidate; candidate.commits = commitNames.size(); candidate.snapshots = snapshotNames.size();
    candidate.leases.reserve(commitNames.size() + snapshotNames.size());
    std::vector<Node> nodes; nodes.reserve(commitNames.size());
    for (const auto& name : commitNames) {
        files::ReadLease lease;
        io = commits.Inspect(name, codec::NamespaceOwner, codec::RecordBytes, lease);
        if (io != files::Result::Ok) return StoredError(io);
        if (lease.Info().spec.kind != files::Kind::CatalogCommit || lease.Info().spec.key != codec::CommitKey)
            return Result::RecoveryRequired;
        std::vector<std::uint8_t> bytes;
        io = lease.ReadAll(bytes);
        if (io != files::Result::Ok) return StoredError(io);
        Node node;
        const auto decoded = codec::Decode(bytes.data(), bytes.size(), node.record);
        if (decoded != codec::Result::Ok)
            return decoded == codec::Result::Io ? Result::Io : Result::RecoveryRequired;
        node.head = lease.Info();
        candidate.metadataBytes += node.head.identity.fileBytes;
        nodes.push_back(std::move(node));
        candidate.leases.push_back(std::move(lease));
    }
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        return a.record.revision < b.record.revision;
    });
    candidate.owner = nodes.front().record.owner;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].record.revision != i+1 || nodes[i].record.owner != candidate.owner ||
            (i && !Equal(nodes[i].record.previous, nodes[i-1].head))) return Result::RecoveryRequired;
    }
    std::map<std::wstring, files::BlobInfo> knownSnapshots;
    for (const auto& name : snapshotNames) {
        files::ReadLease lease;
        io = snapshots.Inspect(name, candidate.owner, MaximumSnapshotBytes, lease);
        if (io != files::Result::Ok) return StoredError(io);
        const auto& info = lease.Info();
        if (info.spec.kind != files::Kind::CatalogSnapshot || info.spec.key != codec::SnapshotKey)
            return Result::RecoveryRequired;
        if (info.identity.fileBytes > MaximumMetadataBytes - candidate.metadataBytes) return Result::Limit;
        candidate.metadataBytes += info.identity.fileBytes;
        knownSnapshots.emplace(name, info);
        candidate.leases.push_back(std::move(lease));
    }
    for (const auto& node : nodes) {
        const auto found = knownSnapshots.find(node.record.snapshot.name);
        if (found == knownSnapshots.end() || !Equal(found->second, node.record.snapshot))
            return Result::RecoveryRequired;
    }
    files::ReadLease latest;
    io = snapshots.Acquire(nodes.back().record.snapshot, latest);
    if (io != files::Result::Ok) return StoredError(io);
    io = latest.ReadAll(candidate.bytes);
    if (io != files::Result::Ok) return StoredError(io);
    candidate.revision = nodes.back().record.revision;
    candidate.head = std::move(nodes.back().head);
    output = std::move(candidate);
    return Result::Ok;
}

Result OpenChildren(const files::Directory& root, bool create,
    files::Directory& snapshots, files::Directory& commits) {
    std::vector<std::wstring> rootNames;
    auto rootResult = root.EnumerateAll(4, rootNames);
    if (rootResult == files::Result::Limit) return Result::RecoveryRequired;
    if (rootResult != files::Result::Ok) return Convert(rootResult);
    for (const auto& name : rootNames)
        if (name != L".lock" && name != L"snapshots" && name != L"commits") return Result::RecoveryRequired;
    const auto a = root.Child(L"snapshots", create, snapshots);
    const auto b = root.Child(L"commits", create, commits);
    if (a == files::Result::Ok && b == files::Result::Ok) return Result::Ok;
    if (a != files::Result::Ok && a != files::Result::Missing) return StoredError(a);
    if (b != files::Result::Ok && b != files::Result::Missing) return StoredError(b);
    // An absent half is only empty if the other half contains no generations.
    if (a == files::Result::Ok || b == files::Result::Ok) {
        std::vector<std::wstring> names;
        const auto result = (a == files::Result::Ok ? snapshots : commits).EnumerateAll(1, names);
        if (result == files::Result::Limit) return Result::RecoveryRequired;
        if (result != files::Result::Ok) return Convert(result);
        if (!names.empty()) return Result::RecoveryRequired;
    }
    return Result::Missing;
}
}

Store::Store(std::filesystem::path absoluteMetadataRoot) : root_(std::move(absoluteMetadataRoot)) {}
void Snapshot::Swap(Snapshot& other) noexcept {
    std::swap(valid_, other.valid_); root_.swap(other.root_); std::swap(revision_, other.revision_);
    owner_.swap(other.owner_); std::swap(head_, other.head_); bytes_.swap(other.bytes_);
}
Result Store::Read(Snapshot& output) const noexcept {
    try {
        files::Directory directory;
        auto io = files::Directory::Open(root_, false, directory);
        if (io != files::Result::Ok) return Convert(io);
        files::FileLock lock;
        io = directory.Lock(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits;
        auto result = OpenChildren(directory, false, snapshots, commits);
        if (result != Result::Ok) return result;
        Loaded loaded;
        result = Load(snapshots, commits, loaded);
        if (result != Result::Ok) return result;
        Snapshot candidate; candidate.root_ = root_; candidate.owner_ = loaded.owner;
        candidate.revision_ = loaded.revision; candidate.head_ = std::move(loaded.head);
        candidate.bytes_ = std::move(loaded.bytes); candidate.valid_ = true;
        candidate.Swap(output);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Store::Initialize(const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    return Publish(nullptr, bytes, output);
}
Result Store::Commit(const Snapshot& expected, const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    return Publish(&expected, bytes, output);
}
Result Store::Publish(const Snapshot* expected, const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    bool commitAttempted = false;
    try {
        if (bytes.size() > MaximumSnapshotBytes) return Result::Limit;
        if (expected && (!expected->valid_ || expected->root_ != root_)) return Result::Conflict;
        // Copy caller-owned bytes before file creation; input may alias output.
        Snapshot candidate; candidate.root_ = root_; candidate.bytes_ = bytes;
        files::Directory directory;
        auto io = files::Directory::Open(root_, !expected, directory);
        if (io != files::Result::Ok) return expected ? StoredError(io) : Convert(io);
        files::FileLock lock;
        io = directory.Lock(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits;
        auto result = OpenChildren(directory, !expected, snapshots, commits);
        if (result != Result::Ok) return expected && result == Result::Missing ? Result::RecoveryRequired : result;
        Loaded current;
        result = Load(snapshots, commits, current);
        if (expected) {
            if (result != Result::Ok) return result == Result::Missing ? Result::RecoveryRequired : result;
            if (expected->revision_ != current.revision || expected->owner_ != current.owner ||
                !Equal(expected->head_, current.head)) return Result::Conflict;
            candidate.owner_ = current.owner;
            candidate.revision_ = current.revision + 1;
        } else {
            if (result == Result::Ok) return Result::Conflict;
            if (result != Result::Missing) return result;
            if (!files::crypto::Random(candidate.owner_) ||
                std::all_of(candidate.owner_.begin(), candidate.owner_.end(), [](auto v) { return v == 0; })) return Result::Io;
            candidate.revision_ = 1;
        }
        const auto additional = 2 * files::HeaderBytes + codec::RecordBytes + candidate.bytes_.size();
        if (current.commits >= MaximumCommits || current.snapshots >= MaximumSnapshots ||
            additional > MaximumMetadataBytes - current.metadataBytes) return Result::Limit;
        files::Spec spec; spec.owner = candidate.owner_; spec.kind = files::Kind::CatalogSnapshot; spec.key = codec::SnapshotKey;
        codec::Record record; record.owner = candidate.owner_; record.revision = candidate.revision_;
        record.previous = current.head;
        io = snapshots.Create(spec, candidate.bytes_.data(), candidate.bytes_.size(), record.snapshot);
        if (io != files::Result::Ok) return Convert(io);
        files::ReadLease newSnapshot;
        io = snapshots.Acquire(record.snapshot, newSnapshot);
        if (io != files::Result::Ok) return StoredError(io);
        std::vector<std::uint8_t> encoded;
        const auto coded = codec::Encode(record, encoded);
        if (coded != codec::Result::Ok) return coded == codec::Result::Io ? Result::Io : Result::Invalid;
        spec.owner = codec::NamespaceOwner; spec.kind = files::Kind::CatalogCommit; spec.key = codec::CommitKey;
        commitAttempted = true;
        io = commits.Create(spec, encoded.data(), encoded.size(), candidate.head_);
        if (io != files::Result::Ok) return Result::Indeterminate;
        candidate.valid_ = true;
        candidate.Swap(output);
        return Result::Ok;
    } catch (...) { return commitAttempted ? Result::Indeterminate : Result::Io; }
}
}
