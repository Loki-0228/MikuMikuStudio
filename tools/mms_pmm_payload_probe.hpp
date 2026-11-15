#pragma once

#include "mms_pmm_directory_probe.hpp"

namespace mikumikustudio::format_probe {

enum class PayloadStatus {
    NotApplicable, ChecksumsOk, ChecksumMismatch, InvalidMetadata, OverlappingRanges,
    BudgetExceeded, IoError
};

struct PayloadResult {
    Result header{};
    PayloadStatus status = PayloadStatus::NotApplicable;
    std::size_t metadataBytesExamined = 0;
    std::size_t payloadBytesExamined = 0;
};

inline constexpr std::uint64_t kMaxPayloadReadBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kPayloadChunkBytes = 64 * 1024;

// Compare the stored CRCs to the raw bytes in every declared block. No
// decoding, file snapshot, or authenticity guarantee is provided.
template<class ReadAt>
PayloadResult VerifyPmmPayloads(std::uint64_t fileSize, ReadAt readAt) {
    using namespace directory_detail;
    std::vector<PayloadRange> ranges;
    const auto metadata = InspectPmmMetadata(fileSize, readAt, &ranges);
    const auto result = [&](PayloadStatus status) {
        return PayloadResult{metadata.header, status, metadata.bytesExamined};
    };
    switch (metadata.status) {
    case DirectoryStatus::Absent: return result(PayloadStatus::NotApplicable);
    case DirectoryStatus::Invalid: return result(PayloadStatus::InvalidMetadata);
    case DirectoryStatus::OverlappingRanges: return result(PayloadStatus::OverlappingRanges);
    case DirectoryStatus::BudgetExceeded: return result(PayloadStatus::BudgetExceeded);
    case DirectoryStatus::IoError: return result(PayloadStatus::IoError);
    case DirectoryStatus::MetadataOk: break;
    }

    // Admit the entire payload workload before the first payload read.
    // Subtracting from the remaining quota cannot overflow.
    auto quota = kMaxPayloadReadBytes;
    for (const auto& range : ranges) {
        if (range.size > quota) return result(PayloadStatus::BudgetExceeded);
        quota -= range.size;
    }

    auto out = result(PayloadStatus::ChecksumsOk);
    std::array<char, kPayloadChunkBytes> buffer;
    for (const auto& range : ranges) {
        auto crc = kCrc32Xor;
        auto offset = range.offset;
        auto remaining = range.size;
        while (remaining) {
            const auto count =
                static_cast<std::size_t>((std::min)(remaining, std::uint64_t{buffer.size()}));
            if (!readAt(offset, buffer.data(), count)) {
                out.status = PayloadStatus::IoError;
                return out;
            }
            crc = UpdateCrc32(crc, buffer.data(), count);
            // The metadata check bounded the end of this range by the TOC.
            offset += count;
            remaining -= count;
            out.payloadBytesExamined += count;
        }
        if ((crc ^ kCrc32Xor) != range.crc) {
            out.status = PayloadStatus::ChecksumMismatch;
            return out;
        }
    }
    return out;
}

}  // namespace mikumikustudio::format_probe
