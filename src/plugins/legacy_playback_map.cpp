#include "mikumikustudio/legacy_playback_map.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mms/legacy_playback_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
thread_local mikumikustudio::MMDApp* editor;
thread_local std::uint64_t lastSnapshot;

static_assert(sizeof(MmsLegacyPlaybackMapV1) == 32);
static_assert(offsetof(MmsLegacyPlaybackMapV1, timelineFrame) == 12);
static_assert(offsetof(MmsLegacyPlaybackMapV1, snapshotId) == 16);
static_assert(offsetof(MmsLegacyPlaybackMapV1, timelineSeconds) == 24);

HANDLE Publish(const MmsLegacyPlaybackMapV1& snapshot) noexcept {
    constexpr DWORD size = sizeof(snapshot);
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr);
    if (!section) return nullptr;

    HANDLE reader = nullptr;
    if (void* writeView = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size)) {
        std::memcpy(writeView, &snapshot, size);
        if (UnmapViewOfFile(writeView)) {
            const HANDLE process = GetCurrentProcess();
            if (!DuplicateHandle(process, section, process, &reader, FILE_MAP_READ, FALSE, 0))
                reader = nullptr;
        }
    }
    const DWORD error = reader ? ERROR_SUCCESS : GetLastError();
    CloseHandle(section);
    SetLastError(error);
    return reader;
}
}

namespace mikumikustudio::legacy_playback_map {
void Attach(MMDApp* app) noexcept { editor = app; }

void Detach(MMDApp* app) noexcept {
    if (editor == app) editor = nullptr;
}
}

extern "C" __declspec(dllexport) void* MMS_CALL MMSAcquireLegacyPlaybackMap(std::uint32_t version) {
    if (version != MMS_LEGACY_PLAYBACK_MAP_VERSION_1) {
        SetLastError(ERROR_REVISION_MISMATCH);
        return nullptr;
    }
    if (!editor || !IsWindow(editor->MainWindow()) || editor->sceneRecoveryPending) {
        SetLastError(ERROR_NOT_READY);
        return nullptr;
    }

    constexpr double framesPerSecond = 30.0;
    const bool playing = editor->PlaybackActive() != 0;
    std::int32_t frame = editor->CurrentFrame();
    double seconds = frame / framesPerSecond;
    if (playing) {
        const double cursor = editor->PlaybackCursorSeconds();
        if (!std::isfinite(cursor)) {
            SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        seconds = std::clamp(cursor, mikumikustudio::frame_limits::minimum / framesPerSecond,
                             mikumikustudio::frame_limits::maximum / framesPerSecond);
        const double scaled = seconds * framesPerSecond;
        const double nearest = std::round(scaled);
        // A cursor initialized by frame / 30 must return that frame even if
        // multiplication rounds down. Preserve truncation for other subframes.
        frame = static_cast<std::int32_t>(nearest / framesPerSecond == seconds ? nearest : scaled);
    }

    const MmsLegacyPlaybackMapV1 snapshot{sizeof(MmsLegacyPlaybackMapV1), version,
        playing ? MMS_LEGACY_PLAYBACK_MAP_PLAYING : 0u, frame, lastSnapshot + 1, seconds};
    const HANDLE handle = Publish(snapshot);
    if (handle) lastSnapshot = snapshot.snapshotId;
    return handle;
}
