#include "mikudancestudio/import_texture_cache.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/d3d_wrapper.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/model_alpha_pass.hpp"
#include "mikudancestudio/texture_lookup.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace mikudancestudio::import_texture_cache {
namespace {
using plugins::resource_budget::Collector;
struct Release { template<class T> void operator()(T* p) const noexcept { if (p) p->Release(); } };
struct Free { void operator()(wchar_t* p) const noexcept { std::free(p); } };
template<class T> using Com = std::unique_ptr<T, Release>;
using Name = std::unique_ptr<wchar_t, Free>;
thread_local Preparation* active = nullptr;
constexpr size_t capacity = 10000;
constexpr UINT d3dxDefault = 0xFFFFFFFFu;

bool FatalLoadResult(HRESULT value) noexcept {
    return value == E_OUTOFMEMORY || value == D3DERR_OUTOFVIDEOMEMORY ||
        value == D3DERR_DEVICELOST || value == D3DERR_DEVICENOTRESET || value == D3DERR_INVALIDCALL ||
        value == D3DERR_NOTAVAILABLE || value == E_NOINTERFACE || value == E_ABORT || value == E_UNEXPECTED ||
        value == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

HRESULT LoadDefault(IDirect3DDevice9* device, const wchar_t* path, IDirect3DTexture9** out) {
    *out = nullptr;
    if (!device) return D3DERR_INVALIDCALL;
    auto& api = d3dx::Get();
    if (!api.Load() || !api.fromFileExW) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    struct ImageInfo { UINT width, height, depth, levels; UINT rest[16]; } info{};
    Com<IDirect3DTexture9> texture;
    const auto load = [&](DWORD usage) {
        // Release a previous failed result too; third-party D3DX hooks are not
        // permission to leak a nonnull failure out parameter.
        texture.reset(); IDirect3DTexture9* next = nullptr;
        HRESULT result;
        try {
            result = api.fromFileExW(device, path, d3dxDefault, d3dxDefault, 1, usage,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, d3dxDefault, d3dxDefault, 0, &info, nullptr, &next);
        } catch (...) { if (next) next->Release(); throw; }
        texture.reset(next); return result;
    };
    auto result = load(1024);
    if (FatalLoadResult(result)) return result;
    if (SUCCEEDED(result) && info.width == 1 && info.height == 1) result = load(0);
    if (FatalLoadResult(result)) return result;
    if (FAILED(result)) result = load(0);
    if (FAILED(result)) return result;
    if (!texture) return E_UNEXPECTED;
    *out = texture.release(); return result;
}

bool LevelBytes(const D3DSURFACE_DESC& desc, uint64_t& bytes) {
    if (!desc.Width || !desc.Height || desc.Type != D3DRTYPE_SURFACE ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE) return false;
    uint64_t columns = desc.Width, rows = desc.Height, stride = 0;
    switch (desc.Format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: stride = 4; break;
    case D3DFMT_R8G8B8: stride = 3; break;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: stride = 2; break;
    case D3DFMT_A8: case D3DFMT_L8: stride = 1; break;
    case D3DFMT_DXT1: stride = 8; columns = (columns + 3) / 4; rows = (rows + 3) / 4; break;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        stride = 16; columns = (columns + 3) / 4; rows = (rows + 3) / 4; break;
    default: return false;
    }
    bytes = Collector::Product(Collector::Product(columns, rows), stride); return true;
}

void MeasureTexture(IUnknown* object, Collector& out) {
    if (!object) return;
    IDirect3DTexture9* raw = nullptr;
    const auto result = object->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&raw));
    Com<IDirect3DTexture9> texture(raw);
    if (FAILED(result) || !texture) { out.MissingResource(plugins::resource_budget::ExternalRenderer); return; }
    IUnknown* token = nullptr;
    const auto query = texture->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&token));
    Com<IUnknown> identity(token);
    if (FAILED(query) || !identity) { out.MissingResource(plugins::resource_budget::RenderQuery); return; }
    const auto levels = texture->GetLevelCount();
    if (!levels || levels > 32) { out.MissingResource(plugins::resource_budget::RenderQuery); return; }
    uint64_t cpu = 0, d3d = 0;
    const auto add = [&](const D3DSURFACE_DESC& desc) {
        uint64_t bytes = 0;
        if (!LevelBytes(desc, bytes)) return false;
        switch (desc.Pool) {
        case D3DPOOL_MANAGED: d3d = Collector::Sum(d3d, Collector::Product(bytes, 2)); break;
        case D3DPOOL_DEFAULT: d3d = Collector::Sum(d3d, bytes); break;
        case D3DPOOL_SYSTEMMEM: case D3DPOOL_SCRATCH: cpu = Collector::Sum(cpu, bytes); break;
        default: return false;
        }
        return true;
    };
    D3DSURFACE_DESC top{};
    if (FAILED(texture->GetLevelDesc(0, &top))) { out.MissingResource(plugins::resource_budget::RenderQuery); return; }
    if (top.Usage & D3DUSAGE_AUTOGENMIPMAP) {
        // GetLevelCount is always 1 for AUTOGEN and the lower levels are not
        // queryable. Reserve the complete chain implied by the queried top
        // dimensions/format, down to 1x1. This is a payload upper bound, not a
        // measurement of driver allocation or of when sublevels are generated.
        // https://learn.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3dbasetexture9-getlevelcount
        if (levels != 1) { out.MissingResource(plugins::resource_budget::RenderQuery); return; }
        auto desc = top;
        for (;;) {
            if (!add(desc)) { out.MissingResource(plugins::resource_budget::RenderQuery); return; }
            if (desc.Width == 1 && desc.Height == 1) break;
            desc.Width = (std::max)(UINT(1), desc.Width / 2);
            desc.Height = (std::max)(UINT(1), desc.Height / 2);
        }
    } else for (UINT level = 0; level < levels; ++level) {
        D3DSURFACE_DESC desc = top;
        if ((level && FAILED(texture->GetLevelDesc(level, &desc))) || !add(desc)) {
            out.MissingResource(plugins::resource_budget::RenderQuery); return;
        }
    }
    if (cpu) out.Add(identity.get(), cpu, 1);
    if (d3d) out.Add(identity.get(), d3d, 1, true);
}
}

namespace detail {
struct Entry {
    std::wstring path;
    size_t slot = 0;
    bool added = false;
    void* name = nullptr;
    IUnknown* texture = nullptr;
    int32_t tag = 0, emptyTag = 0;
    uint64_t nameBytes = 0;
    Com<IUnknown> retainedTexture, cacheReference;
    Name pendingName;
};
struct State {
    D3DRenderer& renderer;
    IDirect3DDevice9* device;
    TextureLoader loader;
    DWORD thread = GetCurrentThreadId();
    uint64_t generation = texture_lookup_detail::generation.load(std::memory_order_relaxed);
    size_t used = 0, added = 0;
    MmsResult status = MMS_OK;
    bool sealed = false, published = false;
    std::vector<Entry> entries;
    State(D3DRenderer& value, TextureLoader function)
        : renderer(value), device(value.device), loader(function ? function : LoadDefault) {
        while (used < capacity && renderer.resourcePool[used].heapBuffer) ++used;
        // Publishing into a hole must not expose previously hidden trailing
        // entries or overwrite an unnamed external COM resource.
        for (size_t index = used; index < capacity; ++index)
            if (renderer.resourcePool[index].heapBuffer || renderer.resourcePool[index].comObject) {
                status = MMS_E_NOT_READY; break;
            }
    }
    MmsResult Fail(MmsResult value) noexcept { if (status == MMS_OK) status = value; return status; }
    bool ReferencesCurrent() const noexcept {
        if (GetCurrentThreadId() != thread || status != MMS_OK || renderer.device != device) return false;
        for (const auto& entry : entries) {
            const auto& live = renderer.resourcePool[entry.slot];
            if (entry.added && !published) {
                if (live.heapBuffer || live.comObject || live.tag != entry.emptyTag) return false;
            } else if (live.heapBuffer != entry.name || live.comObject != entry.texture || live.tag != entry.tag ||
                std::wmemcmp(static_cast<const wchar_t*>(live.heapBuffer), entry.path.c_str(), entry.path.size() + 1) != 0)
                return false;
        }
        return true;
    }
    bool PreparationCurrent() const noexcept {
        return !published && generation == texture_lookup_detail::generation.load(std::memory_order_relaxed) &&
            ReferencesCurrent();
    }
};
}

Retained::Retained(std::shared_ptr<detail::State> value) : state_(std::move(value)) {}
Retained::~Retained() = default;
bool Retained::IsCurrent() const noexcept {
    return state_->published ? state_->ReferencesCurrent() : state_->PreparationCurrent();
}
void Retained::MeasureResources(Collector& out) const {
    out.Array(this, 1); out.Array(state_.get(), 1); out.Vector(state_->entries);
    for (const auto& entry : state_->entries) {
        out.String(entry.path);
        // The renderer owns published names. Their measured allocation remains
        // attributable to this import while its exact cache slot is current.
        if (entry.added) out.Add(entry.name, entry.nameBytes, 1);
        MeasureTexture(entry.retainedTexture.get(), out);
    }
    if (!IsCurrent()) out.MissingResource(plugins::resource_budget::NativeOpaque);
}

Publication::Publication(std::shared_ptr<detail::State> value) : state_(std::move(value)) {}
Publication::~Publication() = default;
bool Publication::IsCurrent() const noexcept { return state_->sealed && state_->PreparationCurrent(); }
void Publication::Publish() noexcept {
    if (state_->published) return;
    for (auto& entry : state_->entries) if (entry.added) {
        auto& live = state_->renderer.resourcePool[entry.slot];
        live.heapBuffer = entry.pendingName.release();
        live.comObject = entry.cacheReference.release();
        live.tag = entry.tag;
    }
    state_->published = true;
    if (state_->added) InvalidateTextureLookup();
}

Preparation::Preparation(D3DRenderer& renderer, TextureLoader loader)
    : state_(std::make_shared<detail::State>(renderer, loader)) {}
Preparation::~Preparation() = default;
MmsResult Preparation::Status() const noexcept {
    if (GetCurrentThreadId() != state_->thread) return MMS_E_THREAD;
    return state_->status;
}
bool Preparation::IsFor(const D3DRenderer& renderer) const noexcept {
    return &state_->renderer == &renderer && !state_->sealed && state_->PreparationCurrent();
}
int Preparation::Load(const wchar_t* path) noexcept {
    if (GetCurrentThreadId() != state_->thread) return 0;
    if (state_->status != MMS_OK) return 0;
    if (state_->sealed) { state_->Fail(MMS_E_BUSY); return 0; }
    if (!path) { state_->Fail(MMS_E_ARGUMENT); return 0; }
    if (!*path) return 0;
    if (!state_->PreparationCurrent()) { state_->Fail(MMS_E_STALE); return 0; }
    try {
        for (const auto& entry : state_->entries) if (entry.path == path) return 1;
        detail::Entry entry; entry.path = path;
        for (size_t index = 0; index < state_->used; ++index) {
            const auto& live = state_->renderer.resourcePool[index];
            if (!live.heapBuffer) { state_->Fail(MMS_E_STALE); return 0; }
            if (std::wcscmp(static_cast<const wchar_t*>(live.heapBuffer), path) != 0) continue;
            entry.slot = index; entry.name = live.heapBuffer; entry.texture = live.comObject; entry.tag = live.tag;
            if (entry.texture) { entry.texture->AddRef(); entry.retainedTexture.reset(entry.texture); }
            state_->entries.push_back(std::move(entry)); return 1;
        }
        if (state_->added >= capacity - state_->used) { state_->Fail(MMS_E_MEMORY); return 0; }
        entry.slot = state_->used + state_->added;
        const auto& destination = state_->renderer.resourcePool[entry.slot];
        if (destination.heapBuffer || destination.comObject) { state_->Fail(MMS_E_NOT_READY); return 0; }
        entry.emptyTag = destination.tag;
        IDirect3DTexture9* raw = nullptr;
        HRESULT result;
        try { result = state_->loader(state_->device, path, &raw); }
        catch (...) { if (raw) raw->Release(); throw; }
        Com<IDirect3DTexture9> texture(raw);
        if (FAILED(result)) {
            if (result == E_OUTOFMEMORY || result == D3DERR_OUTOFVIDEOMEMORY) state_->Fail(MMS_E_MEMORY);
            else if (result == D3DERR_DEVICELOST || result == D3DERR_DEVICENOTRESET ||
                result == D3DERR_INVALIDCALL || result == D3DERR_NOTAVAILABLE || result == E_NOINTERFACE ||
                result == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) state_->Fail(MMS_E_NOT_READY);
            else if (result == E_ABORT) state_->Fail(MMS_E_CANCELLED);
            else if (result == E_UNEXPECTED) state_->Fail(MMS_E_FAILED);
            return 0; // Other D3DX failures retain the legacy optional-image policy.
        }
        if (!texture) { state_->Fail(MMS_E_FAILED); return 0; }
        const auto bytes = Collector::Product(Collector::Sum(entry.path.size(), 1), sizeof(wchar_t));
        if (bytes > (std::numeric_limits<size_t>::max)()) throw std::bad_alloc();
        entry.pendingName.reset(static_cast<wchar_t*>(std::malloc(static_cast<size_t>(bytes))));
        if (!entry.pendingName) throw std::bad_alloc();
        std::memcpy(entry.pendingName.get(), entry.path.c_str(), static_cast<size_t>(bytes));
        entry.name = entry.pendingName.get(); entry.nameBytes = bytes;
        CacheTextureColorAndAlpha(texture.get(), reinterpret_cast<unsigned char*>(&entry.tag));
        entry.texture = texture.get(); entry.retainedTexture.reset(texture.release());
        entry.texture->AddRef(); entry.cacheReference.reset(entry.texture); entry.added = true;
        state_->entries.push_back(std::move(entry)); ++state_->added;
        if (!state_->PreparationCurrent()) { state_->Fail(MMS_E_STALE); return 0; }
        return 1;
    } catch (const std::bad_alloc&) { state_->Fail(MMS_E_MEMORY); }
    catch (...) { state_->Fail(MMS_E_FAILED); }
    return 0;
}
MmsResult Preparation::Finish(std::shared_ptr<Retained>& retained, std::unique_ptr<Publication>& publication) noexcept {
    if (const auto error = Status()) return error;
    if (state_->sealed) return MMS_E_BUSY;
    if (!state_->PreparationCurrent()) return state_->Fail(MMS_E_STALE);
    try {
        auto owner = std::shared_ptr<Retained>(new Retained(state_));
        auto candidate = std::unique_ptr<Publication>(new Publication(state_));
        state_->sealed = true; retained = std::move(owner); publication = std::move(candidate); return MMS_OK;
    } catch (const std::bad_alloc&) { return state_->Fail(MMS_E_MEMORY); }
    catch (...) { return state_->Fail(MMS_E_FAILED); }
}

ScopedCapture::ScopedCapture(Preparation& preparation) noexcept {
    if (active) {
        if (GetCurrentThreadId() == preparation.state_->thread) preparation.state_->Fail(MMS_E_BUSY);
        if (GetCurrentThreadId() == active->state_->thread) active->state_->Fail(MMS_E_BUSY);
        return;
    }
    // A wrong-thread scope must still intercept legacy calls; Load/Status then
    // report the thread violation instead of silently mutating the live cache.
    captured_ = &preparation; active = captured_;
}
ScopedCapture::~ScopedCapture() { if (captured_ && active == captured_) active = nullptr; }
bool TryLoadCaptured(D3DRenderer* renderer, const wchar_t* path, int& result) noexcept {
    if (!active) return false;
    result = 0;
    if (GetCurrentThreadId() != active->state_->thread) return true;
    if (&active->state_->renderer != renderer) { active->state_->Fail(MMS_E_ARGUMENT); return true; }
    result = active->Load(path); return true;
}
}
#endif
