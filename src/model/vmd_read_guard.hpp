#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <io.h>
#include <limits>
#include <vector>
#include "mikumikustudio/original_import.hpp"

namespace mikumikustudio::vmd_read {

// Parse the same immutable bytes that will be imported. A preflight followed
// by unchecked reads from the file would still allow a late I/O failure (or a
// changed file) to leave earlier keyframes and the undo ring half updated.
class Input {
public:
    static constexpr std::size_t absent = (std::numeric_limits<std::size_t>::max)();
    int version = 0;
    std::size_t cameraOffset = absent, lightOffset = absent;
    std::size_t shadowOffset = absent, displayOffset = absent, standardEnd = 0;
    std::uint32_t boneCount = 0, morphCount = 0, displayCount = 0, maximumIkEntries = 0;
    std::uint64_t totalIkEntries = 0;
    std::uint32_t maximumFrame=0, cameraCount=0, lightCount=0, shadowCount=0;

    bool Load(int fd) {
        const auto length = _filelengthi64(fd);
        if (length < 0 || static_cast<std::uint64_t>(length) > bytes_.max_size()) return false;
        bytes_.resize(static_cast<std::size_t>(length));
        std::size_t done = 0;
        while (done < bytes_.size()) {
            const auto chunk = static_cast<unsigned>((std::min)(bytes_.size() - done, std::size_t(1024 * 1024)));
            const int read = _read(fd, bytes_.data() + done, chunk);
            if (read <= 0) return false;
            done += static_cast<std::size_t>(read);
        }
        return Validate();
    }

    int Read(void* destination, unsigned count) {
        if (count > Remaining()) return Remaining() == 0 ? 0 : -1;
        if (count) std::memcpy(destination, bytes_.data() + position_, count);
        position_ += count;
        return static_cast<int>(count);
    }
    std::size_t Tell() const { return position_; }
    std::size_t Size() const { return bytes_.size(); }
    const std::uint8_t* Data(std::size_t offset) const { return bytes_.data() + offset; }
    bool Seek(std::size_t position) {
        if (position > bytes_.size()) return false;
        position_ = position;
        return true;
    }
    // Called only after Validate established all record boundaries. Inspect
    // every name before any key or undo record is applied to the scene.
    bool OriginalNamesFit() const {
        const auto name = [&](std::size_t offset, std::size_t size) {
            return original_import::Legacy(reinterpret_cast<const char*>(Data(offset)), size, size);
        };
        std::size_t at = 30, size = version == 1 ? 10 : 20;
        if (!name(at, size)) return false;
        at += size + 4;
        for (std::uint32_t i = 0; i < boneCount; ++i, at += 111)
            if (!name(at, 15)) return false;
        at += 4;
        for (std::uint32_t i = 0; i < morphCount; ++i, at += 23)
            if (!name(at, 15)) return false;
        if (displayOffset == absent) return true;
        at = displayOffset + 4;
        for (std::uint32_t i = 0; i < displayCount; ++i) {
            std::uint32_t count = 0;
            std::memcpy(&count, Data(at + 5), 4);
            at += 9;
            for (std::uint32_t j = 0; j < count; ++j, at += 21)
                if (!name(at, 20)) return false;
        }
        return true;
    }
    static bool AllocationSize(std::size_t count, std::size_t stride, std::size_t& bytes) {
        if (stride && count > (std::numeric_limits<std::size_t>::max)() / stride) return false;
        bytes = count * stride;
        return true;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_ = 0;
    std::size_t Remaining() const { return bytes_.size() - position_; }
    bool SkipRecords(std::uint32_t count, std::size_t stride) {
        if (count > Remaining() / stride) return false;
        position_ += static_cast<std::size_t>(count) * stride;
        return true;
    }
    bool Section(std::size_t stride, std::uint32_t* result = nullptr, std::size_t frameOffset = 0) {
        std::uint32_t count = 0;
        if (Read(&count, sizeof(count)) != sizeof(count) || count>Remaining()/stride) return false;
        for(std::uint32_t i=0;i<count;++i){std::uint32_t frame;std::memcpy(&frame,bytes_.data()+position_+std::size_t(i)*stride+frameOffset,4);maximumFrame=(std::max)(maximumFrame,frame);}
        if(!SkipRecords(count,stride))return false;
        if (result) *result = count;
        return true;
    }
    bool Finish() { standardEnd = position_; position_ = 0; return true; }
    bool Validate() {
        char header[31]{};
        if (Read(header, 30) != 30) return false;
        if (std::strcmp(header, "Vocaloid Motion Data file") == 0) version = 1;
        else if (std::strcmp(header, "Vocaloid Motion Data 0002") == 0) version = 2;
        else return false;
        if (!SkipRecords(1, version == 1 ? 10 : 20) ||
            !Section(111, &boneCount,15) || !Section(23, &morphCount,15)) return false;
        std::size_t undoBytes = 0;
        // Each imported key can append up to three snapshots; undo.dirty is
        // signed 32-bit even on the 64-bit host.
        if (boneCount > std::uint32_t((std::numeric_limits<std::int32_t>::max)() / 3) ||
            !AllocationSize(boneCount, 192, undoBytes)) return false;

        // Early writers can end at any complete optional section. One to
        // three bytes of a count, or a partial record, is never an empty section.
        if (!Remaining()) return Finish();
        cameraOffset = position_;
        if (!Section(version == 1 ? 36 : 61,&cameraCount)) return false;
        if (!Remaining()) return Finish();
        lightOffset = position_;
        if (!Section(28,&lightCount)) return false;
        if (!Remaining()) return Finish();
        if (version == 2) {
            shadowOffset = position_;
            if (!Section(9,&shadowCount)) return false;
            if (!Remaining()) return Finish();
        }
        displayOffset = position_;
        std::uint32_t count = 0;
        if (Read(&count, sizeof(count)) != sizeof(count) || count > Remaining() / 9) return false;
        displayCount=count;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t entries = 0;
            std::uint32_t frame=0;if(Read(&frame,4)!=4)return false;
            maximumFrame=(std::max)(maximumFrame,frame);
            if (!SkipRecords(1, 1) || Read(&entries, sizeof(entries)) != sizeof(entries) ||
                !SkipRecords(entries, 21)) return false;
            totalIkEntries += entries;
            maximumIkEntries = (std::max)(maximumIkEntries, entries);
        }
        // Extension codecs own the remainder. Their existing policy is to
        // ignore unknown, truncated, or damaged optional tails, not reject the
        // already complete standard motion.
        return Finish();
    }
};

} // namespace mikumikustudio::vmd_read
