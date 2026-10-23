// MAINTENANCE: regression-keep. Real 64MiB aggregate metadata budget and Limit preservation; CPU-only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "mikumikustudio/cache_catalog.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {
namespace fs = std::filesystem;
namespace catalog = mikumikustudio::cache_catalog;
namespace files = mikumikustudio::cache_files;
using Bytes = std::vector<std::uint8_t>;
using Digest = files::Digest;
using Result = catalog::Result;

constexpr std::size_t kMiB = 1024u * 1024u;
constexpr std::size_t kSnapshotBytes = 16u * kMiB;
constexpr std::uint64_t kMetadataBudget = 64ull * kMiB;
constexpr std::uint64_t kEnvelopeBytes = 128;
constexpr std::uint64_t kCommitPayloadBytes = 368;
constexpr std::uint64_t kCommitFileBytes = kEnvelopeBytes + kCommitPayloadBytes;
// A generation adds one snapshot file (128-byte envelope plus payload) and one
// commit file (128-byte envelope plus 368-byte record).
constexpr std::uint64_t kGenerationOverhead = kEnvelopeBytes + kCommitFileBytes;
constexpr std::uint64_t kThreeGenerationBytes = 3ull * (kSnapshotBytes + kGenerationOverhead);
constexpr std::uint64_t kFourthPayloadBytes =
    kMetadataBudget - kThreeGenerationBytes - kGenerationOverhead;
static_assert(kSnapshotBytes == catalog::MaximumSnapshotBytes, "suite targets the 16MiB snapshot limit");
static_assert(kMetadataBudget == catalog::MaximumMetadataBytes, "suite targets the 64MiB metadata budget");
static_assert(kGenerationOverhead == 624, "snapshot envelope plus commit envelope and record");
static_assert(kFourthPayloadBytes == kSnapshotBytes - 2496, "fourth payload is 16MiB - 2496");
static_assert(kThreeGenerationBytes + kGenerationOverhead + kFourthPayloadBytes == kMetadataBudget,
    "four generations exactly fill the metadata budget");

unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
void Check(bool value, const std::string& why) { ++checks; if (!value) throw std::runtime_error(why); }

// Streaming CNG SHA-256 so the suite never holds a second full metadata file in
// memory and never relies on production hashing helpers for its own evidence.
class Sha256 {
public:
    Sha256() {
        try {
        Check(BCryptOpenAlgorithmProvider(&provider_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "open SHA-256 provider");
        ULONG objectBytes = 0;
        ULONG digestBytes = 0;
        ULONG returned = 0;
        Check(BCryptGetProperty(provider_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                  static_cast<ULONG>(sizeof(objectBytes)), &returned, 0) >= 0 &&
              returned == static_cast<ULONG>(sizeof(objectBytes)),
            "read SHA-256 object size");
        Check(BCryptGetProperty(provider_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestBytes),
                  static_cast<ULONG>(sizeof(digestBytes)), &returned, 0) >= 0 &&
              digestBytes == static_cast<ULONG>(Digest{}.size()),
            "read SHA-256 digest size");
        object_.resize(objectBytes);
        Check(BCryptCreateHash(provider_, &hash_, object_.data(), objectBytes, nullptr, 0, 0) >= 0,
            "create SHA-256 hash");
        } catch (...) {
            Reset();
            throw;
        }
    }
    ~Sha256() { Reset(); }
    void Reset() noexcept {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (provider_ != nullptr) BCryptCloseAlgorithmProvider(provider_, 0);
        hash_ = nullptr;
        provider_ = nullptr;
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Update(const void* data, std::size_t size) {
        const auto* cursor = static_cast<const std::uint8_t*>(data);
        while (size != 0) {
            const auto piece = (std::min)(size, std::size_t(1u * kMiB));
            Check(BCryptHashData(hash_, const_cast<PUCHAR>(cursor), static_cast<ULONG>(piece), 0) >= 0,
                "hash bytes");
            cursor += piece;
            size -= piece;
        }
    }
    Digest Finish() {
        Digest digest{};
        Check(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
            "finish SHA-256 hash");
        return digest;
    }

private:
    BCRYPT_ALG_HANDLE provider_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
};

Digest HashBytes(const Bytes& bytes) {
    Sha256 hash;
    hash.Update(bytes.data(), bytes.size());
    return hash.Finish();
}

Digest HashFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "open owned metadata file for independent hashing");
    Sha256 hash;
    std::array<char, 64u * 1024u> buffer{};
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.Update(buffer.data(), static_cast<std::size_t>(count));
        if (input.eof()) break;
        Check(bool(input), "read complete owned metadata file");
    }
    return hash.Finish();
}

std::uint8_t PatternByte(std::uint64_t generation, std::size_t offset) {
    return static_cast<std::uint8_t>(0x5au + generation * 73u + offset * 131u +
        (offset >> 11) * 17u + (offset >> 22) * 5u);
}

void FillPattern(Bytes& bytes, std::uint64_t generation) {
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = PatternByte(generation, i);
}

void CheckPattern(const Bytes& bytes, std::uint64_t generation, const char* why) {
    std::size_t mismatches = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != PatternByte(generation, i)) {
            if (mismatches == 0) first = i;
            ++mismatches;
        }
    }
    Check(mismatches == 0,
        std::string(why) + " first=" + std::to_string(first) + " count=" + std::to_string(mismatches));
}

struct SnapshotView {
    bool valid = false;
    std::uint64_t revision = 0;
    files::Owner owner{};
    std::uint64_t bytes = 0;
    Digest digest{};
};

SnapshotView View(const catalog::Snapshot& snapshot) {
    SnapshotView view;
    view.valid = snapshot.IsValid();
    view.revision = snapshot.Revision();
    view.owner = snapshot.StoreId();
    view.bytes = snapshot.Bytes().size();
    view.digest = HashBytes(snapshot.Bytes());
    return view;
}

bool SameView(const SnapshotView& left, const SnapshotView& right) {
    return left.valid == right.valid && left.revision == right.revision && left.owner == right.owner &&
        left.bytes == right.bytes && left.digest == right.digest;
}

void CheckSameView(const SnapshotView& before, const SnapshotView& after, const char* why) {
    Check(SameView(before, after), std::string(why) + ": output fields or bytes changed");
}

struct FileSig {
    std::wstring relative;
    std::uint64_t size = 0;
    Digest digest{};
};

struct DiskState {
    std::vector<FileSig> files;
    std::uint64_t blobBytes = 0;
    std::uint64_t snapshotFileBytes = 0;
    std::uint64_t commitFileBytes = 0;
    std::size_t snapshots = 0;
    std::size_t commits = 0;
};

bool BlobName(const std::wstring& name) {
    if (name.size() != 37 || name.compare(32, 5, L".blob") != 0) return false;
    for (std::size_t i = 0; i < 32; ++i) {
        if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) return false;
    }
    return true;
}

void AddFile(DiskState& state, const fs::path& root, const fs::path& path) {
    FileSig sig;
    sig.relative = fs::relative(path, root).generic_wstring();
    sig.size = fs::file_size(path);
    sig.digest = HashFile(path);
    state.files.push_back(std::move(sig));
}

// Independent on-disk inventory: canonical names, real file sizes and streamed
// SHA-256 content, never production enumeration or metadata accounting.
DiskState Inspect(const fs::path& root) {
    DiskState state;
    std::vector<std::wstring> names;
    for (const auto& entry : fs::directory_iterator(root)) names.push_back(entry.path().filename().wstring());
    std::sort(names.begin(), names.end());
    Check(names == std::vector<std::wstring>{L".lock", L"commits", L"snapshots"},
        "metadata root holds exactly the lock file and generation directories");
    Check(fs::is_regular_file(root / L".lock"), "metadata root lock is a regular file");
    AddFile(state, root, root / L".lock");
    for (std::size_t kind = 0; kind < 2; ++kind) {
        const bool snapshot = kind == 0;
        const fs::path directory = root / (snapshot ? L"snapshots" : L"commits");
        Check(fs::is_directory(directory), "metadata generation directory exists");
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(directory)) {
            Check(entry.is_regular_file(), "generation directory contains only regular files");
            Check(BlobName(entry.path().filename().wstring()), "generation file uses a canonical blob name");
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            AddFile(state, root, file);
            const auto size = fs::file_size(file);
            state.blobBytes += size;
            if (snapshot) {
                ++state.snapshots;
                state.snapshotFileBytes += size;
                Check(size >= kEnvelopeBytes && size <= kEnvelopeBytes + kSnapshotBytes,
                    "snapshot file is a 128-byte envelope plus at most 16MiB");
            } else {
                ++state.commits;
                state.commitFileBytes += size;
                Check(size == kCommitFileBytes, "commit file is a 128-byte envelope plus the 368-byte record");
            }
        }
    }
    std::sort(state.files.begin(), state.files.end(), [](const FileSig& left, const FileSig& right) {
        return left.relative < right.relative;
    });
    Check(state.files.size() == 1 + state.snapshots + state.commits,
        "inventory covers every metadata file");
    return state;
}

void CheckSameFiles(const DiskState& before, const DiskState& after, const char* why) {
    Check(before.files.size() == after.files.size(), std::string(why) + ": file count changed");
    for (std::size_t i = 0; i < before.files.size() && i < after.files.size(); ++i) {
        const auto& left = before.files[i];
        const auto& right = after.files[i];
        Check(left.relative == right.relative, std::string(why) + ": file name changed");
        Check(left.size == right.size, std::string(why) + ": file size changed");
        Check(left.digest == right.digest, std::string(why) + ": file content changed");
    }
    Check(before.blobBytes == after.blobBytes, std::string(why) + ": blob byte total changed");
}
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        Check(argc == 3 && std::wstring(argv[1]) == L"--store", "usage: --store NEW_ABSOLUTE_DIRECTORY");
        const fs::path root(argv[2]);
        const auto native = root.native();
        Check(root.is_absolute() && root == root.lexically_normal() && root != root.root_path() &&
                  !root.filename().empty(),
            "store root must be a normalized absolute non-root path");
        Check(native.size() > 3 && native[1] == L':' && (native[2] == L'\\' || native[2] == L'/') &&
                  ((native[0] >= L'A' && native[0] <= L'Z') || (native[0] >= L'a' && native[0] <= L'z')),
            "store root must be a new local drive path");
        Check(!fs::exists(root), "store root must not already exist");
        Check(fs::is_directory(root.parent_path()), "store parent directory must exist");
        ULARGE_INTEGER freeBytes{};
        Check(GetDiskFreeSpaceExW(root.parent_path().c_str(), &freeBytes, nullptr, nullptr) != FALSE &&
                  freeBytes.QuadPart >= 80ull * kMiB,
            "real budget fixture requires at least 80MiB free disk space");
        Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "claim the new owned store root");
        std::printf("BUDGET root=%ls\n", root.c_str());

        catalog::Store store(root);
        catalog::Snapshot current;
        Bytes payload(kSnapshotBytes);
        FillPattern(payload, 1);
        const auto initialized = store.Initialize(payload, current);
        std::printf("BUDGET initialize-result=%u\n", static_cast<unsigned>(initialized));
        Check(initialized == Result::Ok, "initialize the first real 16MiB generation");
        Check(current.IsValid() && current.Revision() == 1 && current.StoreId() != files::Owner{},
            "initial snapshot exposes valid identity and revision one");
        Check(current.Bytes().size() == kSnapshotBytes, "initial snapshot stores the full 16MiB payload");
        CheckPattern(current.Bytes(), 1, "initial snapshot bytes follow the deterministic pattern");
        std::printf("BUDGET initialize revision=1 payload=%zu\n", current.Bytes().size());

        for (std::uint64_t generation = 2; generation <= 3; ++generation) {
            FillPattern(payload, generation);
            Check(store.Commit(current, payload, current) == Result::Ok, "commit a real 16MiB generation");
            Check(current.IsValid() && current.Revision() == generation, "committed revision increments");
            Check(current.Bytes().size() == kSnapshotBytes, "committed snapshot keeps the full 16MiB payload");
            CheckPattern(current.Bytes(), generation, "committed snapshot bytes follow the deterministic pattern");
            std::printf("BUDGET commit revision=%llu payload=%zu\n",
                static_cast<unsigned long long>(current.Revision()), current.Bytes().size());
        }
        payload.clear();
        payload.shrink_to_fit();

        const DiskState three = Inspect(root);
        Check(three.snapshots == 3 && three.commits == 3,
            "three generations produced three snapshot and three commit files");
        Check(three.snapshotFileBytes == 3ull * (kSnapshotBytes + kEnvelopeBytes),
            "each snapshot file is 128 bytes plus 16MiB");
        Check(three.commitFileBytes == 3ull * kCommitFileBytes,
            "each commit file is 128 bytes plus the 368-byte record");
        Check(three.blobBytes == kThreeGenerationBytes,
            "three generations occupy 3*(16MiB+624) actual blob bytes");
        std::printf("BUDGET disk generations=3 blobs=%zu bytes=%llu\n",
            three.snapshots + three.commits, static_cast<unsigned long long>(three.blobBytes));

        const SnapshotView threeView = View(current);
        const DiskState beforeOver = three;
        Bytes over(static_cast<std::size_t>(kFourthPayloadBytes + 1));
        FillPattern(over, 4);
        Check(store.Commit(current, over, current) == Result::Limit,
            "one byte above the remaining metadata budget is rejected");
        CheckSameView(View(current), threeView, "over-budget failure");
        CheckPattern(current.Bytes(), 3, "over-budget failure retains the complete revision three payload");
        const DiskState afterOver = Inspect(root);
        CheckSameFiles(beforeOver, afterOver, "over-budget failure");
        over.clear();
        over.shrink_to_fit();
        std::printf("BUDGET over-limit payload=%zu result=Limit unchanged=1\n",
            static_cast<std::size_t>(kFourthPayloadBytes + 1));

        Bytes boundary(static_cast<std::size_t>(kFourthPayloadBytes));
        FillPattern(boundary, 4);
        const auto storeId = current.StoreId();
        const auto boundaryResult = store.Commit(current, boundary, current);
        std::printf("BUDGET boundary-result=%u\n", static_cast<unsigned>(boundaryResult));
        Check(boundaryResult == Result::Ok,
            "the exact remaining metadata budget accepts the fourth generation");
        Check(current.IsValid() && current.Revision() == 4, "boundary generation publishes revision four");
        Check(current.StoreId() == storeId, "boundary generation keeps the store identity");
        Check(current.Bytes().size() == boundary.size(), "boundary generation stores the exact allowed payload");
        CheckPattern(current.Bytes(), 4, "boundary snapshot bytes follow the deterministic pattern");
        boundary.clear();
        boundary.shrink_to_fit();

        const DiskState four = Inspect(root);
        Check(four.snapshots == 4 && four.commits == 4,
            "four generations produced four snapshot and four commit files");
        Check(four.commitFileBytes == 4ull * kCommitFileBytes,
            "boundary commit file keeps the 496-byte envelope and record");
        Check(four.snapshotFileBytes ==
                  3ull * (kSnapshotBytes + kEnvelopeBytes) + (kFourthPayloadBytes + kEnvelopeBytes),
            "snapshot files account for three full generations and the exact boundary payload");
        Check(four.blobBytes == kMetadataBudget, "actual .blob file sizes sum to exactly 64MiB");
        std::printf("BUDGET boundary revision=4 payload=%zu blobs=%zu blob-bytes=%llu budget-exact=1\n",
            static_cast<std::size_t>(kFourthPayloadBytes), four.snapshots + four.commits,
            static_cast<unsigned long long>(four.blobBytes));

        current = catalog::Snapshot{};
        catalog::Snapshot reopened;
        Check(catalog::Store(root).Read(reopened) == Result::Ok, "reopen the exact-budget metadata store");
        Check(reopened.IsValid() && reopened.Revision() == 4 && reopened.StoreId() == storeId,
            "reopened store exposes revision four and the same identity");
        Check(reopened.Bytes().size() == static_cast<std::size_t>(kFourthPayloadBytes),
            "reopened latest snapshot has the exact boundary payload length");
        CheckPattern(reopened.Bytes(), 4, "reopened latest bytes follow the deterministic pattern");
        std::printf("BUDGET reopen revision=4 payload=%zu verified=1\n", reopened.Bytes().size());

        const SnapshotView fourView = View(reopened);
        const DiskState beforeEmpty = four;
        Check(store.Commit(reopened, Bytes{}, reopened) == Result::Limit,
            "an empty fifth generation cannot fit the exhausted metadata budget");
        CheckSameView(View(reopened), fourView, "empty fifth failure");
        CheckPattern(reopened.Bytes(), 4, "empty fifth failure retains the complete revision four payload");
        const DiskState afterEmpty = Inspect(root);
        CheckSameFiles(beforeEmpty, afterEmpty, "empty fifth failure");
        Check(afterEmpty.blobBytes == kMetadataBudget, "empty fifth failure adds no blob bytes");
        std::printf("BUDGET final-empty result=Limit unchanged=1 blob-bytes=%llu\n",
            static_cast<unsigned long long>(afterEmpty.blobBytes));
        std::printf("PASS cache catalog budget checks=%u root=%ls blobs=%zu blob-bytes=%llu revisions=4\n",
            checks, root.c_str(), afterEmpty.snapshots + afterEmpty.commits,
            static_cast<unsigned long long>(afterEmpty.blobBytes));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL cache catalog budget checks=%u: %s\n", checks, error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAIL cache catalog budget unexpected exception\n");
        return 2;
    }
}
