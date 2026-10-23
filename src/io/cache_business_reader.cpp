#include "cache_business_reader.hpp"
#include <new>
#include <utility>

namespace mikumikustudio::cache_business {
struct PublishedIndex::State {
    cache_catalog::Snapshot source;
    Manifest manifest;
    Index index;
    std::vector<cache_files::ReadLease> leases;
};

namespace {
ReadResult FromCodec(Result result) noexcept {
    switch (result) {
    case Result::Ok: return ReadResult::Ok;
    case Result::Invalid: return ReadResult::RecoveryRequired;
    case Result::Unsupported: return ReadResult::Unsupported;
    case Result::Limit: return ReadResult::Limit;
    case Result::AllocationFailure: return ReadResult::AllocationFailure;
    }
    return ReadResult::RecoveryRequired;
}
ReadResult FromFile(cache_files::Result result) noexcept {
    using FileResult = cache_files::Result;
    switch (result) {
    case FileResult::Ok: return ReadResult::Ok;
    case FileResult::Busy: return ReadResult::Busy;
    case FileResult::Io: return ReadResult::Io;
    case FileResult::Limit: return ReadResult::Limit;
    case FileResult::Missing:
    case FileResult::Invalid:
    case FileResult::Corrupt:
    case FileResult::Conflict:
    case FileResult::DeletePending:
        return ReadResult::RecoveryRequired;
    }
    return ReadResult::RecoveryRequired;
}
}

PublishedIndex::PublishedIndex() noexcept = default;
PublishedIndex::~PublishedIndex() = default;
PublishedIndex::PublishedIndex(PublishedIndex&&) noexcept = default;
PublishedIndex& PublishedIndex::operator=(PublishedIndex&&) noexcept = default;

bool PublishedIndex::IsValid() const noexcept { return bool(state_); }
const cache_catalog::Snapshot& PublishedIndex::Source() const noexcept {
    static const cache_catalog::Snapshot empty;
    return state_ ? state_->source : empty;
}
const Index& PublishedIndex::Value() const noexcept {
    static const Index empty;
    return state_ ? state_->index : empty;
}
const Manifest& PublishedIndex::Description() const noexcept {
    static const Manifest empty;
    return state_ ? state_->manifest : empty;
}

ReadResult LoadPublishedIndex(const cache_catalog::Snapshot& source,
    const cache_files::Directory& pages, PublishedIndex& output) noexcept {
    if (!source.IsValid() || !pages.IsValid()) return ReadResult::Invalid;
    try {
        auto candidate = std::make_unique<PublishedIndex::State>();
        const auto& bytes = source.Bytes();
        auto result = DecodeManifest(bytes.data(), bytes.size(), candidate->manifest);
        if (result != Result::Ok) return FromCodec(result);
        if (candidate->manifest.storeOwner != source.StoreId())
            return ReadResult::RecoveryRequired;

        // Copy before publishing: source can be a reference into output itself.
        // All candidate leases remain local until the final noexcept swap.
        candidate->source = source;
        candidate->leases.reserve(candidate->manifest.pages.size());
        std::vector<Page> decoded;
        decoded.reserve(candidate->manifest.pages.size());
        for (const auto& ref : candidate->manifest.pages) {
            cache_files::ReadLease lease;
            auto io = pages.Acquire(ref.blob, lease);
            if (io != cache_files::Result::Ok) return FromFile(io);
            Bytes payload;
            io = lease.ReadAll(payload);
            if (io != cache_files::Result::Ok) return FromFile(io);
            Page page;
            result = DecodePage(payload.data(), payload.size(), page);
            if (result != Result::Ok) return FromCodec(result);
            decoded.push_back(std::move(page));
            candidate->leases.push_back(std::move(lease));
        }
        result = Assemble(candidate->manifest, decoded, candidate->index);
        if (result != Result::Ok) return FromCodec(result);

        output.state_.swap(candidate);
        return ReadResult::Ok;
    } catch (const std::bad_alloc&) {
        return ReadResult::AllocationFailure;
    } catch (...) {
        return ReadResult::Io;
    }
}
}
