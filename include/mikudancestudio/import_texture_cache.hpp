#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mms/plugin.h>
#include <d3d9.h>
#include <memory>

namespace mikudancestudio {
class D3DRenderer;
namespace plugins::resource_budget { class Collector; }
namespace import_texture_cache {
namespace detail { struct State; }

// A successful loader transfers one texture reference through out. Even on
// failure a nonnull out is released by Preparation. The default implements
// the legacy D3DX format, pool, retry and colour-sampling policy.
using TextureLoader = HRESULT (*)(IDirect3DDevice9*, const wchar_t*, IDirect3DTexture9** out);

class Retained final {
    std::shared_ptr<detail::State> state_;
    explicit Retained(std::shared_ptr<detail::State>);
    friend class Preparation;
public:
    ~Retained();
    // The renderer and its cache must outlive this owner and its history.
    // Unrelated appends are allowed; replacement/reset of a referenced slot
    // rejects instead of silently reopening a source file on redo.
    bool IsCurrent() const noexcept;
    // Adds owned containers, known new cache-name allocations and referenced
    // texture payloads by allocation/COM identity. A shared Collector dedupes
    // resources across owners. Existing cache-name capacity, driver metadata
    // and unrelated global cache allocations are outside this owner's charge.
    // AUTOGEN hides lower mip descriptors: its charge is the full-chain payload
    // upper bound derived from the queried top-level dimensions and format.
    // Unsupported resources retain missing flags; no estimated fallback size.
    void MeasureResources(plugins::resource_budget::Collector&) const;
};

class Publication final {
    std::shared_ptr<detail::State> state_;
    explicit Publication(std::shared_ptr<detail::State>);
    friend class Preparation;
public:
    ~Publication();
    bool IsCurrent() const noexcept;
    // Caller validates every participant before the first shared-state write.
    // Transfers prepared names/references into free tail slots; no allocation,
    // file access, callbacks or UI. Published cache entries remain owned by the
    // renderer across object undo/redo and normal history eviction.
    void Publish() noexcept;
};

class Preparation final {
    std::shared_ptr<detail::State> state_;
    friend class ScopedCapture;
    friend bool TryLoadCaptured(D3DRenderer*, const wchar_t*, int&) noexcept;
public:
    explicit Preparation(D3DRenderer&, TextureLoader = nullptr);
    ~Preparation();
    Preparation(const Preparation&) = delete;
    Preparation& operator=(const Preparation&) = delete;
    // Exact case-sensitive cache matching, including duplicates in this batch.
    // A missing/invalid optional image returns 0 with Status()==MMS_OK.
    // Memory/capacity/device errors, stale state or exceptions are sticky fatal
    // errors: the importer must inspect Status before accepting its object.
    int Load(const wchar_t*) noexcept;
    MmsResult Status() const noexcept;
    bool IsFor(const D3DRenderer&) const noexcept;
    // Freezes without publishing. Both outputs stay unchanged on failure.
    // Cancelling all batch holders releases private names and COM references.
    // The caller keeps Retained in the import record and charges MeasureResources before
    // calling Publication::Publish together with the object's publication.
    MmsResult Finish(std::shared_ptr<Retained>&, std::unique_ptr<Publication>&) noexcept;
};

// Optional bridge for loaders which still call LoadTextureShared. Explicit new
// importers can pass Preparation& and call Load directly. Main thread only;
// nesting or a different renderer is a fatal error, never a legacy fallthrough.
class ScopedCapture final {
    Preparation* captured_ = nullptr;
public:
    explicit ScopedCapture(Preparation&) noexcept;
    ~ScopedCapture();
    ScopedCapture(const ScopedCapture&) = delete;
    ScopedCapture& operator=(const ScopedCapture&) = delete;
};
bool TryLoadCaptured(D3DRenderer*, const wchar_t*, int& result) noexcept;
}
}
#endif
