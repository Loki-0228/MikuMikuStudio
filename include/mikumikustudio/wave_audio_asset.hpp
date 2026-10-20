#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/wave_audio_context.hpp"
#include <memory>
#include <string>

namespace mikumikustudio::wave_audio {
class Asset;
using AssetPtr = std::shared_ptr<const Asset>;
// UI-thread retained PCM. The source name is metadata; playback and history
// use an installation-local private file backed by the captured PCM bytes.
class Asset {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Asset(std::unique_ptr<Impl>);
public:
    ~Asset();
    static AssetPtr Decode(const wchar_t* source, std::uint64_t maximumBytes);
    static AssetPtr Capture(const WaveAudioContext&, const wchar_t* source, std::uint64_t maximumBytes);
    const wchar_t* Source() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    bool Equals(const Asset&) const noexcept;
    friend class Prepared;
};
// Holds all fallible allocations, file opens and sound-buffer creation before
// publishing. The live WaveAudioContext address remains stable for consumers.
class Prepared {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Prepared(WaveAudioContext&, AssetPtr);
    ~Prepared();
    bool IsCurrent() const noexcept;
    void Publish() noexcept;
    std::uint64_t ResourceBytes() const noexcept;
};
AssetPtr Active(const WaveAudioContext*) noexcept;
const wchar_t* Source(const WaveAudioContext*) noexcept;
bool NeedsRefresh(const WaveAudioContext*) noexcept;
void Refreshed(const WaveAudioContext*) noexcept;
// Call only after closing the live file/feed resources, including on reset.
void Forget(WaveAudioContext*) noexcept;
bool Stopped(const WaveAudioContext&) noexcept;
}
#endif
