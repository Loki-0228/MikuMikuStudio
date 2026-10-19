// CPU-only production private-loader checks. Include in native history tests.
#include "mikumikustudio/accessory_import.hpp"
#include "mikumikustudio/import_texture_cache.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <string>
#include <vector>
namespace {
namespace ai = accessory_import;
struct AccessoryImportContext {
    unsigned meshes = 0, buffers = 0, calls = 0, normalized = 0;
    HRESULT result = S_OK, textureResult = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    bool normalize = true, shortBuffer = false, nullBuffer = false, throwNormalize = false;
    const char* texture = nullptr;
    IDirect3DDevice9* device = reinterpret_cast<IDirect3DDevice9*>(std::uintptr_t(0x20000));
    std::wstring loadedPath;
    std::vector<std::wstring> texturePaths;
};
thread_local AccessoryImportContext* accessoryImportContext = nullptr;
struct AccessoryImportMesh final : IUnknown {
    AccessoryImportContext& context;
    ULONG refs = 1;
    explicit AccessoryImportMesh(AccessoryImportContext& c) : context(c) { ++c.meshes; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto result = --refs;
        if (!result) { --context.meshes; delete this; }
        return result;
    }
};
struct AccessoryImportBuffer final : IUnknown {
    struct Material { D3DMATERIAL9 value; char* name; } material{};
    AccessoryImportContext& context;
    ULONG refs = 1;
    explicit AccessoryImportBuffer(AccessoryImportContext& c) : context(c) {
        ++c.buffers; material.value.Diffuse = { .8f, .6f, .4f, .75f };
        material.value.Specular = { .3f, .2f, .1f, 1.f };
        material.value.Power = 12.f; material.name = const_cast<char*>(c.texture);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto result = --refs;
        if (!result) { --context.buffers; delete this; }
        return result;
    }
    virtual void* STDMETHODCALLTYPE GetBufferPointer() { return &material; }
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() { return context.shortBuffer ? 1u : DWORD(sizeof material); }
};
HRESULT WINAPI AccessoryImportLoad(LPCWSTR path, DWORD flags, IDirect3DDevice9* device,
    void**, void** materials, void**, DWORD* count, void** mesh) {
    auto& c = *accessoryImportContext;
    Check(flags == 544 && device == c.device, "private loader retains native D3DX options and owning device");
    ++c.calls; c.loadedPath = path;
    *mesh = new AccessoryImportMesh(c);
    if (!c.nullBuffer) *materials = new AccessoryImportBuffer(c);
    *count = 1;
    return c.result;
}
bool AccessoryImportNormalize(void*& mesh, IDirect3DDevice9* device) {
    auto& c = *accessoryImportContext; ++c.normalized;
    Check(mesh != nullptr && device == c.device, "private normalizer receives retained mesh");
    if (c.throwNormalize) throw std::bad_alloc();
    return c.normalize;
}
HRESULT AccessoryImportMissingTexture(IDirect3DDevice9* device, const wchar_t* path, IDirect3DTexture9** out) {
    auto& c = *accessoryImportContext;
    Check(device == c.device, "private texture preparation uses the same device");
    *out = nullptr; c.texturePaths.emplace_back(path); return c.textureResult;
}
struct AccessoryImportFixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    AccessoryImportContext context;
    ai::testing::ScopedMeshApi backend{{AccessoryImportLoad, AccessoryImportNormalize}};
    std::wstring directory, nested, mesh, vac, oldCwd;
    std::vector<unsigned char> stateBefore, rendererBefore;
    std::size_t poolsBefore = 0;
    explicit AccessoryImportFixture() {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        std::memset(renderer.get(), 0, sizeof *renderer);
        accessoryImportContext = &context; renderer->device = context.device;
        app->Renderer() = renderer.get(); app->CurrentFrame() = 137; app->SceneModified() = 7;
        app->SelectedAccessorySlot() = 42; app->SetSelectedModelSlot(3); app->CameraMode() = 1;
        wcscpy_s(app->PathWorkspace().projectDirectory, L"Z:\\untouched-project\\");
        wcscpy_s(app->PathWorkspace().resolvedPath, L"Z:\\untouched-resolved.x");
        wchar_t temp[MAX_PATH]{}, name[MAX_PATH]{}, current[32768]{};
        Check(GetTempPathW(MAX_PATH, temp) != 0 && GetTempFileNameW(temp, L"mai", 0, name) != 0,
              "private accessory temporary path");
        Check(DeleteFileW(name) && CreateDirectoryW(name, nullptr), "private accessory temporary directory");
        directory = std::wstring(name) + L"\\"; nested = directory + L"sub\\";
        Check(CreateDirectoryW(nested.c_str(), nullptr) != FALSE, "private accessory nested mesh directory");
        mesh = nested + L"中文附件.x"; vac = directory + L"placement.VAC";
        Write(mesh, "xof 0303txt 0032\n");
        Check(GetCurrentDirectoryW(32768, current) != 0, "private accessory captures process directory");
        oldCwd = current;
        stateBefore.resize(sizeof app->state); std::memcpy(stateBefore.data(), &app->state, stateBefore.size());
        rendererBefore.resize(sizeof *renderer); std::memcpy(rendererBefore.data(), renderer.get(), rendererBefore.size());
        std::lock_guard<std::mutex> lock(mdl::KeyPoolStorage<mdl::AccessoryKey>::mutex);
        poolsBefore = mdl::KeyPoolStorage<mdl::AccessoryKey>::capacities.size();
    }
    static void Write(const std::wstring& path, const std::string& bytes) {
        FILE* file = nullptr;
        Check(!_wfopen_s(&file, path.c_str(), L"wb") && file, "private accessory writes input fixture");
        Check(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "private accessory fixture contents written");
        Check(std::fclose(file) == 0, "private accessory fixture closed");
    }
    void Vac(const char* shadow = "1\n") {
        std::string name;
        Check(text_encoding::EncodeExact(L"sub\\中文附件.x", CP_UTF8, name), "private VAC mesh UTF8 path");
        Write(vac, "unused label\n" + std::string("\xEF\xBB\xBF") + name +
            "\n2.5\n1.25,-2.5,3.75\n0.1,0.2,-0.3\nignored bone name\n" + shadow);
    }
    void Unchanged() {
        wchar_t current[32768]{};
        Check(GetCurrentDirectoryW(32768, current) && oldCwd == current,
              "private accessory leaves process working directory unchanged");
        Check(std::memcmp(stateBefore.data(), &app->state, stateBefore.size()) == 0,
              "private accessory leaves slots keys UI selection dirty paths and app state unchanged");
        Check(std::memcmp(rendererBefore.data(), renderer.get(), rendererBefore.size()) == 0,
              "private accessory leaves renderer and shared texture pool unchanged");
    }
    void Empty() {
        Check(context.meshes == 0 && context.buffers == 0, "private accessory releases every temporary COM resource");
        std::lock_guard<std::mutex> lock(mdl::KeyPoolStorage<mdl::AccessoryKey>::mutex);
        Check(mdl::KeyPoolStorage<mdl::AccessoryKey>::capacities.size() == poolsBefore,
              "private accessory cancellation unregisters its private key pool");
    }
    ~AccessoryImportFixture() {
        Empty(); app->Renderer() = nullptr; accessoryImportContext = nullptr;
        DeleteFileW(vac.c_str()); DeleteFileW(mesh.c_str()); RemoveDirectoryW(nested.c_str()); RemoveDirectoryW(directory.c_str());
    }
};
void AccessoryImportDefaultsChecks() {
    AccessoryImportFixture f;
    {
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        Check(ai::Prepare(*f.app, f.mesh.c_str(), textures, prepared) == ai::Result::Ready, "private X accessory preparation succeeds");
        auto* object = prepared.Object(); auto* keys = prepared.Keys();
        Check(object && keys && object != f.app->AccessorySlot(0) && keys != f.app->AccessoryKeys(0),
              "private X owns resources outside all live scene slots");
        Check(mdl::KeyPoolIdentity(keys) && mdl::KeyPoolCapacity(keys) == 10000,
              "private X owns an independently registered native-capacity key pool");
        Check(object->visible == 1 && object->parentModel == -1 && object->parentBone == 0 &&
              object->scale == 1.f && object->opacity == 1.f && object->shadowEnabled == 0 &&
              object->currentMaterial == -1 && object->additiveBlend == 0,
              "private X preserves native accessory defaults");
        Check(keys[0].frame == 0 && keys[0].next == 0 && keys[0].previous == 0 &&
              keys[0].parentModel == -1 && keys[0].visible == 1 && keys[0].scale == 1.f && keys[0].opacity == 1.f,
              "private X root key contains all initial authored fields");
        Check(text_encoding::Display(object->name) == L"中文附件.x" && f.context.loadedPath == f.mesh &&
              object->sourcePath == f.mesh && object->directory == f.nested,
              "private X preserves Unicode object and source paths");
        const auto& material = *static_cast<D3DMATERIAL9*>(object->materials);
        Check(material.Diffuse.r == .8f * .1f && material.Ambient.r == .8f && material.Power == 12.f &&
              material.Specular.g == .2f * .1f, "private material conversion matches legacy fixed-function values");
        Check(f.context.buffers == 0 && f.context.meshes == 1, "private X releases D3DX material buffer after copying");
        const auto calls = f.context.calls;
        Check(ai::Prepare(*f.app, f.mesh.c_str(), textures, prepared) == ai::Result::NotReady &&
              prepared.Object() == object && prepared.Keys() == keys && f.context.calls == calls,
              "nonempty private output cannot be overwritten or trigger another load");
        f.Unchanged();
        auto* oldObject = object; auto* oldKeys = keys;
        ai::Prepared moved(std::move(prepared));
        Check(!prepared.Object() && !prepared.Keys() && moved.Object() == oldObject && moved.Keys() == oldKeys,
              "private accessory move transfers sole ownership");
        mdl::AccessoryRecord* released = nullptr; mdl::AccessoryKey* releasedKeys = nullptr;
        failAfter = 0; moved.Release(released, releasedKeys); failAfter = -1;
        Check(released == oldObject && releasedKeys == oldKeys && !moved.Object() && !moved.Keys(),
              "private accessory release transfers both resources without allocation");
        DisposeAccessory(released); ::operator delete(released); mdl::KeyPoolDestroy(releasedKeys);
    }
    f.Empty(); f.Unchanged();
    for (const char* shadow : {"1\n", "0\n", "\n\n// older VAC without shadow field\n"}) {
        f.Vac(shadow);
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        Check(ai::Prepare(*f.app, f.vac.c_str(), textures, prepared) == ai::Result::Ready, "private VAC preparation succeeds");
        const auto& o = *prepared.Object(); const auto& k = prepared.Keys()[0];
        Check(o.scale == 2.5f && o.position[1] == -2.5f && o.rotation[2] == -.3f &&
              o.parentModel == -1 && o.parentBone == 0 && o.shadowEnabled == (shadow[0] != '1'),
              "private VAC placement shadow default and unattached parent match legacy import");
        Check(std::memcmp(k.position, o.position, sizeof k.position) == 0 &&
              std::memcmp(k.rotation, o.rotation, sizeof k.rotation) == 0 &&
              k.scale == o.scale && k.opacity == o.opacity && k.shadowEnabled == o.shadowEnabled &&
              k.visible == o.visible && k.parentModel == o.parentModel && k.parentBone == o.parentBone,
              "private VAC copies complete placement into the root key");
        Check(o.sourcePath == f.vac && o.directory == f.nested && f.context.loadedPath == f.mesh,
              "private VAC source remains descriptor while mesh resolves against its own directory");
        f.Unchanged();
    }
    f.Empty();
}
void AccessoryImportTextureChecks() {
    AccessoryImportFixture f; f.Vac();
    for (const char* texture : {"color.bmp", "sphere.sph", "sphere.spa", "color.bmp*sphere.spa", "screen.bmp"}) {
        f.context.texture = texture; f.context.texturePaths.clear();
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        Check(ai::Prepare(*f.app, f.vac.c_str(), textures, prepared) == ai::Result::Ready && textures.Status() == MMS_OK,
              "missing optional accessory textures do not fail private import");
        const auto type = prepared.Object()->textureTypes[0];
        Check(type == (std::strstr(texture, "*") ? 5 : std::strstr(texture, "screen") ? 3 :
              std::strstr(texture, ".sph") ? 1 : std::strstr(texture, ".spa") ? 2 : 0),
              "private accessory base sphere and screen texture flags match legacy behavior");
        const auto expectedCount = std::strstr(texture, "*") ? 2u : std::strstr(texture, "screen") ? 0u : 1u;
        Check(f.context.texturePaths.size() == expectedCount, "private importer routes only required textures to its preparation");
        for (const auto& path : f.context.texturePaths)
            Check(path.compare(0, f.nested.size(), f.nested) == 0, "VAC textures are resolved relative to actual nested X file");
        f.Unchanged();
    }
    f.Empty();
    for (HRESULT error : {E_OUTOFMEMORY, D3DERR_DEVICELOST}) {
        f.context.texture = "color.bmp"; f.context.textureResult = error;
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        Check(ai::Prepare(*f.app, f.mesh.c_str(), textures, prepared) == ai::Result::TextureFailure &&
              textures.Status() != MMS_OK && !prepared.Object() && !prepared.Keys(),
              "fatal texture preparation error cancels private object instead of treating it as a missing image");
        f.Empty(); f.Unchanged();
    }
}
void AccessoryImportFailureChecks() {
    AccessoryImportFixture f;
    for (unsigned mode = 0; mode < 7; ++mode) {
        f.context.result = mode == 0 ? E_FAIL : mode == 5 ? E_OUTOFMEMORY : mode == 6 ? D3DERR_DEVICELOST : S_OK;
        f.context.shortBuffer = mode == 1; f.context.nullBuffer = mode == 2;
        f.context.normalize = mode != 3; f.context.throwNormalize = mode == 4;
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        Check(ai::Prepare(*f.app, f.mesh.c_str(), textures, prepared) ==
              (mode == 4 || mode == 5 ? ai::Result::Memory : mode == 6 ? ai::Result::NotReady : ai::Result::InvalidMesh),
              "private mesh and material failures are reported");
        Check(!prepared.Object() && !prepared.Keys(), "private preparation failure leaves output empty");
        f.Empty(); f.Unchanged();
    }
    f.context.result = S_OK; f.context.shortBuffer = f.context.nullBuffer = f.context.throwNormalize = false;
    f.context.normalize = true;
    {
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        ai::Prepared prepared;
        const auto calls = f.context.calls;
        Check(ai::Prepare(*f.app, L"relative.x", textures, prepared) == ai::Result::InvalidPath &&
              ai::Prepare(*f.app, nullptr, textures, prepared) == ai::Result::InvalidPath &&
              ai::Prepare(*f.app, (f.directory + L"missing.x").c_str(), textures, prepared) == ai::Result::NotFound &&
              f.context.calls == calls, "invalid and missing input is refused before D3DX without dialogs");
        auto otherRenderer = std::make_unique<D3DRenderer>(); std::memset(otherRenderer.get(), 0, sizeof *otherRenderer);
        otherRenderer->device = f.context.device;
        import_texture_cache::Preparation other(*otherRenderer, AccessoryImportMissingTexture);
        Check(ai::Prepare(*f.app, f.mesh.c_str(), other, prepared) == ai::Result::NotReady,
              "private loader refuses a texture batch for another renderer");
    }
    for (const char* scalar : {"nan", "garbage", "1e999"}) {
        std::string path; Check(text_encoding::EncodeExact(f.mesh.c_str(), CP_UTF8, path), "bad VAC source path encoding");
        AccessoryImportFixture::Write(f.vac, "label\n" + std::string("\xEF\xBB\xBF") + path + "\n" + scalar +
            "\n0,0,0\n0,0,0\nnone\n");
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture); ai::Prepared prepared;
        const auto calls = f.context.calls;
        Check(ai::Prepare(*f.app, f.vac.c_str(), textures, prepared) == ai::Result::InvalidVac && f.context.calls == calls,
              "invalid nonfinite or overflowing VAC pose is refused before D3DX");
        f.Empty(); f.Unchanged();
    }
    bool reached = false; unsigned failures = 0; bool sawMeshAllocationFailure = false;
    for (long allowance = 0; allowance < 256; ++allowance) {
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture); ai::Prepared prepared;
        const auto calls = f.context.calls;
        failAfter = allowance; const auto result = ai::Prepare(*f.app, f.mesh.c_str(), textures, prepared); failAfter = -1;
        f.Unchanged();
        if (result == ai::Result::Ready) {
            reached = true;
            failAfter = 0; prepared = ai::Prepared{}; failAfter = -1;
            f.Empty(); break;
        }
        ++failures; sawMeshAllocationFailure = sawMeshAllocationFailure || f.context.calls > calls;
        Check(result == ai::Result::Memory && !prepared.Object() && !prepared.Keys(),
              "allocation failure leaves no partially prepared accessory");
        f.Empty();
    }
    Check(reached && failures > 3 && sawMeshAllocationFailure,
          "private accessory fault sweep covers path mesh material-buffer and key-pool ownership");
}
void AccessoryImportChecks() {
    AccessoryImportDefaultsChecks(); AccessoryImportTextureChecks(); AccessoryImportFailureChecks();
    std::puts("PASS private accessory X/VAC preparation, texture isolation, initial keys and failure cleanup");
}
}
