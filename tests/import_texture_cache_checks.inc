#include "mikumikustudio/import_texture_cache.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/texture_prepare_cache.hpp"
#include <limits>
#include <stdexcept>

namespace {
std::vector<unsigned char> ImportCachePixels(IUnknown* object) {
    IDirect3DTexture9* texture = nullptr;
    Check(object && SUCCEEDED(object->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) && texture,
        "query cached texture");
    D3DSURFACE_DESC desc{};
    Check(SUCCEEDED(texture->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_A8R8G8B8 &&
        desc.Width > 0 && desc.Height > 0 && desc.Width <= 4096 && desc.Height <= 4096, "cached texture dimensions");
    D3DLOCKED_RECT locked{};
    Check(SUCCEEDED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)), "lock cached texture");
    std::vector<unsigned char> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
            static_cast<const unsigned char*>(locked.pBits) + static_cast<ptrdiff_t>(y) * locked.Pitch,
            static_cast<size_t>(desc.Width) * 4);
    texture->UnlockRect(0);
    texture->Release();
    return pixels;
}
void ImportTextureDiskCacheChecks() {
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"MMS_IMPORT_CACHE_TEST_DIR", root, 32768);
    Check(length && length < 32768, "isolated import cache test directory provided");
    const std::wstring dir(root), managerDir = dir + L"\\cache", cacheDir = managerDir + L"\\Textures";
    const std::wstring settings = dir + L"\\settings.json", path = dir + L"\\fixture.tga";
    Check(CreateDirectoryW(dir.c_str(), nullptr) != 0, "create fresh import cache test directory");
    Check(SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR", cacheDir.c_str()) &&
        SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR", managerDir.c_str()) &&
        SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS", settings.c_str()) &&
        SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", nullptr) &&
        SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE", nullptr), "set isolated cache configuration");
    std::vector<unsigned char> data(18 + 37 * 25 * 4, 0);
    data[2] = 2; data[12] = 37; data[14] = 25; data[16] = 32; data[17] = 0x28;
    for (size_t i = 18; i < data.size(); i += 4) {
        data[i] = static_cast<unsigned char>(i); data[i + 1] = 54; data[i + 2] = 97; data[i + 3] = 127;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create real texture fixture");
    DWORD written = 0; const bool wrote = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(file); Check(wrote, "write real texture fixture");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION); Check(d3d != nullptr, "create D3D9");
    HWND window = CreateWindowW(L"STATIC", L"texture import test", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden test window");
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)), "create hidden D3D device");
    auto renderer = std::make_unique<D3DRenderer>(); renderer->device = device;
    auto mutablePath = std::vector<wchar_t>(path.begin(), path.end()); mutablePath.push_back(0);
    const auto before = texture_prepare_cache::GetStatistics();
    Check(LoadTextureShared(reinterpret_cast<unsigned char*>(renderer.get()), mutablePath.data()) == 1, "ordinary load seeds disk cache");
    const auto seeded = texture_prepare_cache::GetStatistics();
    Check(seeded.stores > before.stores, "ordinary load persisted decoded texture");
    auto& entry = renderer->resourcePool[0];
    const auto expectedTag = entry.tag;
    const auto expectedPixels = ImportCachePixels(entry.comObject);
    std::free(entry.heapBuffer); entry.comObject->Release(); entry = {};
    InvalidateTextureLookup();
    {
        import_texture_cache::Preparation preparation(*renderer);
        import_texture_cache::ScopedCapture capture(preparation);
        Check(LoadTextureShared(reinterpret_cast<unsigned char*>(renderer.get()), mutablePath.data()) == 1,
            "cancelled transaction loads cached texture");
        const auto hit = texture_prepare_cache::GetStatistics();
        Check(hit.hits > seeded.hits && hit.nativeCalls == seeded.nativeCalls,
            "cancelled transaction reuses ordinary disk cache");
        Check(!entry.heapBuffer && !entry.comObject && entry.tag == 0,
            "cancelled transaction does not publish texture or tag");
        std::shared_ptr<import_texture_cache::Retained> retained;
        std::unique_ptr<import_texture_cache::Publication> publication;
        Check(preparation.Finish(retained, publication) == MMS_OK && publication && publication->IsCurrent(),
            "freeze texture transaction before cancellation");
        // Dropping both handles without Publish cancels the staged import.
    }
    Check(!entry.heapBuffer && !entry.comObject && entry.tag == 0,
        "cancelled transaction leaves no public texture or tag");
    const auto cancelled = texture_prepare_cache::GetStatistics();
    {
        import_texture_cache::Preparation preparation(*renderer);
        import_texture_cache::ScopedCapture capture(preparation);
        Check(LoadTextureShared(reinterpret_cast<unsigned char*>(renderer.get()), mutablePath.data()) == 1, "transaction import loads cached texture");
        const auto hit = texture_prepare_cache::GetStatistics();
        Check(hit.hits > cancelled.hits && hit.nativeCalls == cancelled.nativeCalls, "transaction import reuses ordinary disk cache after cancellation");
        Check(!renderer->resourcePool[0].heapBuffer && !renderer->resourcePool[0].comObject, "transaction texture remains private before commit");
        std::shared_ptr<import_texture_cache::Retained> retained;
        std::unique_ptr<import_texture_cache::Publication> publication;
        Check(preparation.Finish(retained, publication) == MMS_OK && publication->IsCurrent(), "freeze texture transaction");
        publication->Publish();
        Check(renderer->resourcePool[0].comObject && renderer->resourcePool[0].tag == expectedTag, "transaction publishes matching texture metadata");
        Check(ImportCachePixels(renderer->resourcePool[0].comObject) == expectedPixels, "transaction publishes matching texture pixels");
    }
    std::free(entry.heapBuffer); entry.comObject->Release(); entry = {}; InvalidateTextureLookup();
    renderer.reset(); device->Release(); DestroyWindow(window); d3d->Release();
}
struct ImportTextureTestState { unsigned calls = 0, live = 0, destroyed = 0; };
thread_local ImportTextureTestState* importTextureTestState = nullptr;
class ImportTestTexture final : public IDirect3DTexture9 {
    ULONG references_ = 1;
    ImportTextureTestState& state_;
    bool badDescription_, badFormat_;
    DWORD pixels_[16]{};
public:
    UINT width = 4, height = 4, levels = 2;
    DWORD usage = 0;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    HRESULT textureQueryResult = S_OK;
    bool nullTextureQuery = false, throwTextureQuery = false, invalidateTextureQuery = false, throwIdentityQuery = false;
    ULONG ReferenceCount() const noexcept { return references_; }
    ImportTestTexture(ImportTextureTestState& state, bool badDescription = false, bool badFormat = false)
        : state_(state), badDescription_(badDescription), badFormat_(badFormat) {
        ++state_.live;
        for (auto& pixel : pixels_) pixel = 0xFF112233;
        pixels_[12] = 0x7F112233;
    }
    ~ImportTestTexture() { --state_.live; ++state_.destroyed; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IDirect3DTexture9) &&
            id != __uuidof(IDirect3DBaseTexture9) && id != __uuidof(IDirect3DResource9)) return E_NOINTERFACE;
        if (id == __uuidof(IDirect3DTexture9)) {
            if (invalidateTextureQuery) InvalidateTextureLookup();
            if (!nullTextureQuery) { *out = static_cast<IDirect3DTexture9*>(this); AddRef(); }
            if (throwTextureQuery) throw std::bad_alloc();
            return textureQueryResult;
        }
        *out = static_cast<IDirect3DTexture9*>(this); AddRef();
        if (id == IID_IUnknown && throwIdentityQuery) throw std::bad_alloc();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return levels; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return E_NOTIMPL; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_NONE; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* out) override {
        if (badDescription_ || level >= levels || level >= 32 || !out) return E_FAIL;
        *out = {}; out->Width = (std::max)(UINT(1), width >> level);
        out->Height = (std::max)(UINT(1), height >> level); out->Usage = usage;
        out->Format = badFormat_ ? D3DFMT_UNKNOWN : format;
        out->Type = D3DRTYPE_SURFACE; out->Pool = D3DPOOL_MANAGED; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT, IDirect3DSurface9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* out, const RECT*, DWORD) override {
        if (level || !out || width != 4 || height != 4) return E_NOTIMPL;
        out->Pitch = 16; out->pBits = pixels_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return E_NOTIMPL; }
};
class ImportUnknownTexture final : public IUnknown {
    ULONG references_ = 1;
    ImportTextureTestState& state_;
public:
    explicit ImportUnknownTexture(ImportTextureTestState& state) : state_(state) { ++state_.live; }
    ~ImportUnknownTexture() { --state_.live; ++state_.destroyed; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = nullptr; if (id != IID_IUnknown) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
};
HRESULT ImportTestTextureLoader(IDirect3DDevice9*, const wchar_t* path, IDirect3DTexture9** out) {
    auto& state = *importTextureTestState; ++state.calls; *out = nullptr;
    if (std::wcsstr(path, L"missing")) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (std::wcsstr(path, L"device-lost")) return D3DERR_DEVICELOST;
    if (std::wcsstr(path, L"null-success")) return S_OK;
    *out = new ImportTestTexture(state, std::wcsstr(path, L"bad-query") != nullptr,
        std::wcsstr(path, L"bad-format") != nullptr);
    if (std::wcsstr(path, L"failure-out")) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (std::wcsstr(path, L"throw-oom")) throw std::bad_alloc();
    if (std::wcsstr(path, L"throw-error")) throw std::runtime_error("texture loader failure");
    if (std::wcsstr(path, L"memory-out")) return E_OUTOFMEMORY;
    return S_OK;
}
struct ImportTextureFixture {
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    ImportTextureTestState state;
    ImportTextureTestState* previous = importTextureTestState;
    ImportTextureFixture() {
        renderer->device = reinterpret_cast<IDirect3DDevice9*>(std::uintptr_t(1));
        importTextureTestState = &state; InvalidateTextureLookup();
    }
    void Clear() {
        InvalidateTextureLookup();
        for (auto& entry : renderer->resourcePool) {
            std::free(entry.heapBuffer); if (entry.comObject) entry.comObject->Release(); entry = {};
        }
    }
    ~ImportTextureFixture() { Clear(); importTextureTestState = previous; }
    void External(const wchar_t* path, IUnknown* texture) {
        auto& entry = renderer->resourcePool[0]; const auto bytes = (std::wcslen(path) + 1) * sizeof(wchar_t);
        entry.heapBuffer = std::malloc(bytes); Check(entry.heapBuffer != nullptr, "allocate external texture name fixture");
        std::memcpy(entry.heapBuffer, path, bytes); entry.comObject = texture; InvalidateTextureLookup();
    }
};

void ImportTexturePrivateChecks() {
    using namespace import_texture_cache;
    ImportTextureFixture f;
    const auto generation = texture_lookup_detail::generation.load();
    auto preparation = std::make_unique<Preparation>(*f.renderer, ImportTestTextureLoader);
    Check(preparation->IsFor(*f.renderer), "texture batch belongs to its renderer and device");
    Check(preparation->Load(L"D:\\private\\one.png") == 1 && preparation->Load(L"D:\\private\\one.png") == 1 &&
        f.state.calls == 1 && f.state.live == 1 && !f.renderer->resourcePool[0].heapBuffer,
        "private duplicate texture loads once without publishing a cache slot");
    Check(preparation->Load(L"D:\\private\\missing.png") == 0 && preparation->Status() == MMS_OK,
        "normal missing optional texture is not a fatal batch error");
    Check(preparation->Load(L"D:\\private\\failure-out.png") == 0 && preparation->Status() == MMS_OK && f.state.live == 1,
        "failed optional texture releases a nonnull COM result");
    std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
    Check(preparation->Finish(retained, publication) == MMS_OK && publication->IsCurrent() && retained->IsCurrent() &&
        !preparation->IsFor(*f.renderer), "freeze private texture batch without publishing");
    plugins::resource_budget::Collector measured; retained->MeasureResources(measured);
    Check(measured.Result().Complete() && measured.Result().d3d9Bytes == (4 * 4 + 2 * 2) * 4 * 2 &&
        measured.Result().cpuBytes >= 2 * sizeof(L"D:\\private\\one.png"),
        "texture budget measures every real mip and owned path allocations");
    const auto total = measured.Result().totalBytes; retained->MeasureResources(measured);
    Check(measured.Result().totalBytes == total, "repeated texture measurement deduplicates allocation identities");
    Check(texture_lookup_detail::generation.load() == generation && !f.renderer->resourcePool[0].heapBuffer,
        "texture prepare and measurement leave live lookup generation unchanged");
    preparation.reset(); publication.reset();
    Check(f.state.live == 1, "retained texture owner alone keeps a cancelled batch valid");
    retained.reset();
    Check(f.state.live == 0 && !f.renderer->resourcePool[0].heapBuffer,
        "cancelling all batch holders releases private textures and leaves cache untouched");
}

void ImportTexturePublicationChecks() {
    using namespace import_texture_cache;
    ImportTextureFixture f;
    std::shared_ptr<Retained> first, second, reuse;
    std::unique_ptr<Publication> publishFirst, publishSecond, publishReuse;
    {
        Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.Load(L"D:\\private\\first.png") == 1 && p.Finish(first, publishFirst) == MMS_OK,
            "prepare first retained texture publication");
    }
    failAfter = 0; const bool ready = publishFirst->IsCurrent(); if (ready) publishFirst->Publish(); failAfter = -1;
    Check(ready && first->IsCurrent() && !publishFirst->IsCurrent() && f.renderer->resourcePool[0].comObject &&
        std::wcscmp(static_cast<const wchar_t*>(f.renderer->resourcePool[0].heapBuffer), L"D:\\private\\first.png") == 0,
        "texture publication transfers cache ownership without allocation");
    const auto* tag = reinterpret_cast<const unsigned char*>(&f.renderer->resourcePool[0].tag);
    Check(tag[0] == 0x11 && tag[1] == 0x22 && tag[2] == 0x33 &&
        tag[3] == (kTextureAlphaKnown | kTextureUsesAlpha), "private loader preserves colour and alpha metadata");
    publishFirst.reset();
    {
        Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.Load(L"D:\\private\\second.png") == 1 && p.Finish(second, publishSecond) == MMS_OK,
            "prepare unrelated cache append");
    }
    publishSecond->Publish(); publishSecond.reset();
    Check(first->IsCurrent() && second->IsCurrent(), "unrelated successful append does not invalidate earlier import history");
    const auto calls = f.state.calls;
    {
        Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.Load(L"D:\\private\\first.png") == 1 && p.Finish(reuse, publishReuse) == MMS_OK && f.state.calls == calls,
            "existing cache is reused without loading source again");
    }
    plugins::resource_budget::Collector combined;
    first->MeasureResources(combined); reuse->MeasureResources(combined);
    Check(combined.Result().Complete() && combined.Result().d3d9Bytes == 160,
        "two import owners charge one shared texture only once in a combined budget");
    const auto generation = texture_lookup_detail::generation.load();
    failAfter = 0; publishReuse->Publish(); failAfter = -1; publishReuse.reset();
    Check(generation == texture_lookup_detail::generation.load() && first->IsCurrent() && reuse->IsCurrent(),
        "publishing reuse-only batch does not rewrite cache or invalidate lookup");
    f.Clear();
    Check(!first->IsCurrent() && !second->IsCurrent() && !reuse->IsCurrent() && f.state.live == 2,
        "renderer cache clear invalidates history references while retained owners keep COM resources alive");
    plugins::resource_budget::Collector stale; first->MeasureResources(stale);
    Check(!stale.Result().Complete(), "removed cache entry cannot be reported as a complete import dependency");
    first.reset(); Check(f.state.live == 2, "another import owner retains shared texture after cache clear");
    reuse.reset(); Check(f.state.live == 1, "last shared import owner releases the first texture");
    second.reset(); Check(f.state.live == 0, "last retained import owner releases second texture");
}

void ImportTextureRejectionChecks() {
    using namespace import_texture_cache;
    {
        ImportTextureFixture f;
        Preparation first(*f.renderer, ImportTestTextureLoader), second(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> a, b; std::unique_ptr<Publication> pa, pb;
        Check(first.Load(L"D:\\private\\first.png") == 1 && first.Finish(a, pa) == MMS_OK &&
            second.Load(L"D:\\private\\second.png") == 1 && second.Finish(b, pb) == MMS_OK,
            "two private candidates can prepare against the same empty cache tail");
        f.renderer->resourcePool[0].tag = 12;
        Check(!pa->IsCurrent(), "cache tail metadata mutation invalidates private publication");
        f.renderer->resourcePool[0].tag = 0;
        f.renderer->device = reinterpret_cast<IDirect3DDevice9*>(std::uintptr_t(2));
        Check(!pa->IsCurrent(), "renderer device replacement invalidates prepared texture publication");
        f.renderer->device = reinterpret_cast<IDirect3DDevice9*>(std::uintptr_t(1));
        Check(pb->IsCurrent(), "restore fixture before competing cache publication"); pb->Publish();
        Check(!pa->IsCurrent() && b->IsCurrent() &&
            std::wcscmp(static_cast<const wchar_t*>(f.renderer->resourcePool[0].heapBuffer), L"D:\\private\\second.png") == 0,
            "competing append makes stale candidate reject instead of overwriting cache");
    }
    {
        ImportTextureFixture f;
        wchar_t borrowed[] = L"full-cache-existing.png";
        for (auto& entry : f.renderer->resourcePool) entry.heapBuffer = borrowed;
        {
            Preparation full(*f.renderer, ImportTestTextureLoader);
            Check(full.Load(borrowed) == 1 && full.Load(L"new-at-capacity.png") == 0 &&
                full.Status() == MMS_E_MEMORY && !f.state.calls,
                "full cache still reuses existing names but rejects additions as fatal capacity failure");
        }
        for (auto& entry : f.renderer->resourcePool) entry.heapBuffer = nullptr;
    }
    {
        ImportTextureFixture f;
        auto* texture = new ImportTestTexture(f.state);
        texture->levels = 1; texture->usage = D3DUSAGE_AUTOGENMIPMAP;
        f.External(L"existing-autogen.png", texture);
        Preparation p(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(p.Load(L"existing-autogen.png") == 1 && p.Finish(retained, publication) == MMS_OK,
            "retain auto-mip texture reporting only one accessible level");
        plugins::resource_budget::Collector report; retained->MeasureResources(report);
        Check(report.Result().Complete() && report.Result().d3d9Bytes == (4 * 4 + 2 * 2 + 1) * 4 * 2,
            "AUTOGEN reserves full derived mip payload despite GetLevelCount equal to one");
        texture->width = texture->height = (std::numeric_limits<UINT>::max)();
        bool overflow = false;
        try { plugins::resource_budget::Collector large; retained->MeasureResources(large); }
        catch (const std::bad_alloc&) { overflow = true; }
        Check(overflow, "texture width height and format multiplication rejects payload overflow");
        texture->width = texture->height = 4;
    }
    {
        ImportTextureFixture f;
        auto* texture = new ImportTestTexture(f.state);
        texture->width = 7; texture->height = 5; texture->levels = 1; texture->format = D3DFMT_DXT1;
        f.External(L"existing-blocks.dds", texture);
        Preparation p(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(p.Load(L"existing-blocks.dds") == 1 && p.Finish(retained, publication) == MMS_OK,
            "retain odd-sized block compressed texture");
        plugins::resource_budget::Collector report; retained->MeasureResources(report);
        Check(report.Result().Complete() && report.Result().d3d9Bytes == 2 * 2 * 8 * 2,
            "compressed texture budget rounds each dimension to complete blocks");
    }
    {
        ImportTextureFixture f;
        f.External(L"hidden-tail.png", new ImportTestTexture(f.state));
        f.renderer->resourcePool[2] = f.renderer->resourcePool[0]; f.renderer->resourcePool[0] = {};
        Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.Status() == MMS_E_NOT_READY && !p.Load(L"new.png") && !f.state.calls,
            "malformed cache hole refuses to expose hidden external tail entries");
    }
    {
        ImportTextureFixture f;
        f.External(L"external-unknown.png", new ImportUnknownTexture(f.state));
        Preparation p(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(p.Load(L"external-unknown.png") == 1 && p.Finish(retained, publication) == MMS_OK && !f.state.calls,
            "existing external cache object is reused without speculative casts or reload");
        plugins::resource_budget::Collector report; retained->MeasureResources(report);
        Check((report.Result().missing & plugins::resource_budget::ExternalRenderer) != 0,
            "unknown external COM texture remains explicitly incomplete");
    }
    for (const auto* path : {L"bad-query.png", L"bad-format.png"}) {
        ImportTextureFixture f; Preparation p(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(p.Load(path) == 1 && p.Finish(retained, publication) == MMS_OK, "prepare unmeasurable texture fixture");
        plugins::resource_budget::Collector report; retained->MeasureResources(report);
        Check(!report.Result().Complete() && !report.Result().d3d9Bytes,
            "failed descriptor or unsupported texture format never receives invented payload size");
    }
    for (const auto* path : {L"throw-oom.png", L"memory-out.png", L"throw-error.png", L"device-lost.png"}) {
        ImportTextureFixture f; Preparation p(*f.renderer, ImportTestTextureLoader);
        const auto expected = std::wcsstr(path, L"oom") || std::wcsstr(path, L"memory") ? MMS_E_MEMORY :
            std::wcsstr(path, L"device") ? MMS_E_NOT_READY : MMS_E_FAILED;
        Check(!p.Load(path) && p.Status() == expected && !f.state.live && !p.Load(L"later.png") && f.state.calls == 1,
            "fatal texture failures release returned references and remain sticky");
    }
}

void ImportTextureAllocationChecks() {
    using namespace import_texture_cache;
    ImportTextureFixture f;
    const auto* path = L"D:\\private\\a-long-path-to-exercise-owned-name-allocations.png";
    const long counter = (std::numeric_limits<long>::max)();
    long allocations = 0;
    {
        Preparation p(*f.renderer, ImportTestTextureLoader);
        failAfter = counter; const auto result = p.Load(path); allocations = counter - failAfter; failAfter = -1;
        Check(result == 1 && p.Status() == MMS_OK && allocations > 0, "calibrate private texture load allocations");
    }
    for (long limit = 0; limit <= allocations; ++limit) {
        {
            Preparation p(*f.renderer, ImportTestTextureLoader);
            failAfter = limit; const auto loaded = p.Load(path); failAfter = -1;
            Check((loaded == 1 && p.Status() == MMS_OK) || (!loaded && p.Status() == MMS_E_MEMORY),
                "private texture allocation failure reports memory instead of optional missing image");
            Check(!f.renderer->resourcePool[0].heapBuffer && !f.renderer->resourcePool[0].comObject,
                "every private texture allocation stage leaves renderer cache untouched");
        }
        Check(!f.state.live, "cancelled allocation attempt releases all texture references");
    }
    {
        Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.Load(path) == 1, "prepare texture for freeze allocation calibration");
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        failAfter = counter; const auto result = p.Finish(retained, publication); allocations = counter - failAfter; failAfter = -1;
        Check(result == MMS_OK && allocations > 0, "calibrate texture freeze allocations");
    }
    for (long limit = 0; limit <= allocations; ++limit) {
        {
            Preparation baseline(*f.renderer, ImportTestTextureLoader);
            std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
            Check(baseline.Finish(retained, publication) == MMS_OK, "create freeze output sentinels");
            auto* priorRetained = retained.get(); auto* priorPublication = publication.get();
            Preparation p(*f.renderer, ImportTestTextureLoader); Check(p.Load(path) == 1, "prepare texture freeze failure fixture");
            failAfter = limit; const auto result = p.Finish(retained, publication); failAfter = -1;
            Check(result == MMS_OK || (result == MMS_E_MEMORY && retained.get() == priorRetained &&
                publication.get() == priorPublication && publication->IsCurrent()),
                "freeze allocation failures preserve both outputs and earlier candidate");
            Check(!f.renderer->resourcePool[0].heapBuffer, "freeze allocation sweep never publishes a cache slot");
        }
        Check(!f.state.live, "failed or cancelled freeze releases private texture batch");
    }
}

void ImportTextureCaptureChecks() {
    using namespace import_texture_cache;
    ImportTextureFixture f;
    {
        Preparation p(*f.renderer, ImportTestTextureLoader); wchar_t path[] = L"captured-texture.png";
        { ScopedCapture capture(p);
          Check(LoadTextureShared(reinterpret_cast<unsigned char*>(f.renderer.get()), path) == 1 &&
              !f.renderer->resourcePool[0].heapBuffer, "real legacy texture entry routes explicit capture into private batch"); }
        int result = 5;
        Check(!TryLoadCaptured(f.renderer.get(), path, result) && result == 5,
            "leaving capture restores unbound legacy dispatch without loading anything");
    }
    {
        Preparation p(*f.renderer, ImportTestTextureLoader); auto other = std::make_unique<D3DRenderer>();
        ScopedCapture capture(p); wchar_t path[] = L"wrong-renderer.png";
        Check(!LoadTextureShared(reinterpret_cast<unsigned char*>(other.get()), path) && p.Status() == MMS_E_ARGUMENT &&
            !other->resourcePool[0].heapBuffer, "capture rejects another renderer instead of falling through to legacy loader");
    }
    {
        Preparation first(*f.renderer, ImportTestTextureLoader), second(*f.renderer, ImportTestTextureLoader);
        ScopedCapture outer(first);
        { ScopedCapture inner(second);
          Check(first.Status() == MMS_E_BUSY && second.Status() == MMS_E_BUSY,
              "nested import texture captures are explicit fatal failures"); }
        int result = 1;
        Check(TryLoadCaptured(f.renderer.get(), L"nested.png", result) && !result,
            "rejected nested capture preserves outer interception until its guard ends");
    }
}

void ImportTextureRequiredChecks() {
    using namespace import_texture_cache;
    for (const auto* path : {static_cast<const wchar_t*>(nullptr), L""}) {
        ImportTextureFixture f; Preparation p(*f.renderer, ImportTestTextureLoader);
        Check(p.LoadRequired(path) == MMS_E_ARGUMENT && p.Status() == MMS_E_ARGUMENT &&
            p.LoadRequired(L"later.png") == MMS_E_ARGUMENT && !f.state.calls,
            "required texture rejects null/empty paths before any loader call and preserves the first error");
    }
    for (const auto* path : {L"required-missing.png", L"required-failure-out.png", L"null-success.png"}) {
        ImportTextureFixture f;
        {
            Preparation optional(*f.renderer, ImportTestTextureLoader);
            const auto status = std::wcsstr(path, L"null-success") ? MMS_E_FAILED : MMS_OK;
            Check(!optional.Load(path) && optional.Status() == status && !f.state.live,
                "required support preserves optional missing-image and loader-success-null policies");
        }
        const auto calls = f.state.calls;
        Preparation required(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(required.LoadRequired(path) == MMS_E_FAILED && required.Status() == MMS_E_FAILED &&
            !required.Load(L"later.png") && required.LoadRequired(L"later.png") == MMS_E_FAILED &&
            required.Finish(retained, publication) == MMS_E_FAILED && !retained && !publication &&
            f.state.calls == calls + 1 && !f.state.live && !f.renderer->resourcePool[0].heapBuffer,
            "missing required images make the private batch fatal without leaking failure output or publishing");
    }
    {
        ImportTextureFixture f; f.External(L"named-null.png", nullptr);
        Preparation optional(*f.renderer, ImportTestTextureLoader);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        Check(optional.Load(L"named-null.png") == 1 && optional.Finish(retained, publication) == MMS_OK,
            "optional named-null cache entries retain their legacy acceptance");
        plugins::resource_budget::Collector optionalBudget; retained->MeasureResources(optionalBudget);
        Check(optionalBudget.Result().Complete() && !optionalBudget.Result().d3d9Bytes,
            "optional retained null cache entries remain outside required-resource missing flags");
        Preparation required(*f.renderer, ImportTestTextureLoader);
        Check(required.LoadRequired(L"named-null.png") == MMS_E_NOT_READY && required.Status() == MMS_E_NOT_READY &&
            !f.state.calls && !f.renderer->resourcePool[0].comObject,
            "a cache name alone cannot satisfy a required texture or trigger a speculative reload");
        plugins::resource_budget::Collector requiredBudget; MeasureTextureResources(nullptr, requiredBudget);
        Check(!requiredBudget.Result().Complete() && (requiredBudget.Result().missing & plugins::resource_budget::RenderQuery),
            "direct required null-resource measurement explicitly reports missing data");
    }
    {
        ImportTextureFixture f; f.External(L"unknown-required.png", new ImportUnknownTexture(f.state));
        {
            Preparation required(*f.renderer, ImportTestTextureLoader);
            Check(required.LoadRequired(L"unknown-required.png") == MMS_E_NOT_READY && required.Status() == MMS_E_NOT_READY &&
                !f.state.calls && f.state.live == 1,
                "IUnknown-only cache entries cannot satisfy the texture interface requirement");
            plugins::resource_budget::Collector budget; MeasureTextureResources(f.renderer->resourcePool[0].comObject, budget);
            Check(!budget.Result().Complete() && (budget.Result().missing & plugins::resource_budget::ExternalRenderer),
                "direct unknown-COM measurement uses the same incomplete budget rule as retained owners");
        }
        f.Clear(); Check(!f.state.live && f.state.destroyed == 1, "failed required cache reuse releases its retained COM reference");
    }
    for (unsigned failure = 0; failure != 4; ++failure) {
        ImportTextureFixture f; auto* texture = new ImportTestTexture(f.state); f.External(L"unusable-required.png", texture);
        texture->nullTextureQuery = failure == 0;
        texture->textureQueryResult = failure == 1 ? E_NOINTERFACE : S_OK;
        texture->throwTextureQuery = failure == 2;
        texture->invalidateTextureQuery = failure == 3;
        {
            Preparation required(*f.renderer, ImportTestTextureLoader);
            const auto expected = failure == 2 ? MMS_E_MEMORY : failure == 3 ? MMS_E_STALE : MMS_E_NOT_READY;
            Check(required.LoadRequired(L"unusable-required.png") == expected && required.Status() == expected &&
                required.LoadRequired(L"later.png") == expected && !f.state.calls && texture->ReferenceCount() == 2,
                "required QI null/failure/exception and identity changes are sticky and release every temporary interface");
        }
        Check(texture->ReferenceCount() == 1, "required rejection releases private retained ownership but leaves the cache owner intact");
        f.Clear(); Check(!f.state.live && f.state.destroyed == 1, "unusable required cache resources release exactly once at final ownership exit");
    }
    for (bool identity : {false, true}) {
        ImportTextureFixture f; auto* texture = new ImportTestTexture(f.state); f.External(L"query-exception.png", texture);
        texture->throwTextureQuery = !identity; texture->throwIdentityQuery = identity;
        const auto references = texture->ReferenceCount();
        plugins::resource_budget::Collector budget; bool thrown = false;
        try { MeasureTextureResources(texture, budget); } catch (const std::bad_alloc&) { thrown = true; }
        const auto missing = identity ? plugins::resource_budget::RenderQuery : plugins::resource_budget::ExternalRenderer;
        Check(thrown && texture->ReferenceCount() == references && !budget.Result().Complete() &&
            (budget.Result().missing & missing) && !budget.Result().totalBytes,
            "texture/interface identity QI exceptions release nonnull out references and explicitly invalidate incomplete measurement");
        texture->throwTextureQuery = texture->throwIdentityQuery = false;
        plugins::resource_budget::Collector recovered; MeasureTextureResources(texture, recovered);
        Check(recovered.Result().Complete() && recovered.Result().d3d9Bytes == 160 && texture->ReferenceCount() == references,
            "measurement can recover after either QI exception without leaked COM references");
        f.Clear(); Check(!f.state.live && f.state.destroyed == 1,
            "both query exception paths return to sole source ownership and release its final reference exactly once");
    }
    {
        ImportTextureFixture f; auto* texture = new ImportTestTexture(f.state); f.External(L"existing-required.png", texture);
        std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
        {
            Preparation required(*f.renderer, ImportTestTextureLoader);
            Check(required.LoadRequired(L"existing-required.png") == MMS_OK && required.LoadRequired(L"existing-required.png") == MMS_OK &&
                required.Finish(retained, publication) == MMS_OK && !f.state.calls && texture->ReferenceCount() == 2,
                "real fake IDirect3DTexture9 cache reuse succeeds without disk reads or duplicate retained references");
        }
        plugins::resource_budget::Collector budget;
        retained->MeasureResources(budget); const auto before = budget.Result().totalBytes;
        MeasureTextureResources(texture, budget);
        Check(budget.Result().Complete() && budget.Result().d3d9Bytes == 160 && budget.Result().totalBytes == before &&
            texture->ReferenceCount() == 2, "public texture measurement shares COM identity and mip accounting with retained measurement");
        bool memory = false; plugins::resource_budget::Collector failedBudget;
        failAfter = 0;
        try { MeasureTextureResources(texture, failedBudget); } catch (const std::bad_alloc&) { memory = true; }
        failAfter = -1;
        Check(memory && texture->ReferenceCount() == 2, "budget allocation failure releases both queried COM interfaces");
        const auto generation = texture_lookup_detail::generation.load();
        failAfter = 0; const bool current = publication->IsCurrent(); if (current) publication->Publish(); failAfter = -1;
        Check(current && retained->IsCurrent() && texture_lookup_detail::generation.load() == generation,
            "required reuse shares the original allocation-free no-rewrite publication");
        publication.reset(); retained.reset(); Check(texture->ReferenceCount() == 1 && !f.state.calls,
            "required reused resource returns to sole renderer ownership after cancelling holders");
    }
    {
        ImportTextureFixture f;
        {
            Preparation required(*f.renderer, ImportTestTextureLoader);
            Check(required.LoadRequired(L"private-required.png") == MMS_OK && required.LoadRequired(L"private-required.png") == MMS_OK &&
                f.state.calls == 1 && f.state.live == 1 && !f.renderer->resourcePool[0].heapBuffer,
                "required private duplicates use the same unpublished batch and load only once");
            std::shared_ptr<Retained> retained; std::unique_ptr<Publication> publication;
            Check(required.Finish(retained, publication) == MMS_OK && publication->IsCurrent(), "freeze a required private texture for cancellation");
        }
        Check(!f.state.live && f.state.destroyed == 1 && !f.renderer->resourcePool[0].heapBuffer && !f.renderer->resourcePool[0].comObject,
            "cancelled required preparation releases both private COM references without publishing the renderer cache");
    }
    for (const auto* path : {L"throw-oom.png", L"memory-out.png", L"throw-error.png", L"device-lost.png"}) {
        ImportTextureFixture f; Preparation p(*f.renderer, ImportTestTextureLoader);
        const auto expected = std::wcsstr(path, L"oom") || std::wcsstr(path, L"memory") ? MMS_E_MEMORY :
            std::wcsstr(path, L"device") ? MMS_E_NOT_READY : MMS_E_FAILED;
        Check(p.LoadRequired(path) == expected && p.Status() == expected && !f.state.live &&
            p.LoadRequired(L"later.png") == expected && f.state.calls == 1,
            "required loading preserves existing device/memory/exception codes and their sticky cleanup");
    }
}

void ImportTextureCacheChecks() {
    ImportTexturePrivateChecks();
    ImportTexturePublicationChecks();
    ImportTextureRejectionChecks();
    ImportTextureAllocationChecks();
    ImportTextureCaptureChecks();
    ImportTextureRequiredChecks();
}
}
