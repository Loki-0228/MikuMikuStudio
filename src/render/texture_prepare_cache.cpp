#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/texture_prepare_cache.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/cache_storage_policy.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/parallel_prepare_policy.hpp"
#include "texture_cpu_decode.hpp"
#include "../exports/effect_compile_cache.hpp"
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <condition_variable>
#include <thread>

namespace mikumikustudio::texture_prepare_cache {
namespace {
using Microsoft::WRL::ComPtr;
using Bytes = std::vector<unsigned char>;
using Digest = std::array<unsigned char, 32>;
namespace fs = std::filesystem;
constexpr DWORD kMagic = 0x4354504d; // MPTC
constexpr DWORD kVersion = 2; // native-compatible parallel decoder policy
constexpr std::uint64_t kMaxPixels = 128ull * 1024 * 1024;
constexpr std::uint64_t kMaxDisk = 512ull * 1024 * 1024;
constexpr unsigned kMaxEntries = 128;
thread_local bool insideHostLoad = false;
std::atomic<std::uint64_t> hits{0}, nativeCalls{0}, stores{0}, serial{0};
std::atomic<d3dx::FnCreateTexFromFileExW> original32{nullptr}, original43{nullptr};
std::mutex installMutex;
HMODULE patchedModule = nullptr;
std::atomic<std::uint64_t> staged{0}, decoded{0}, preparedUses{0}, diskReady{0}, prepareFailures{0};

struct Info { UINT width, height, depth, mipLevels, format, resourceType, fileFormat; };
static_assert(sizeof(Info) == 28, "D3DXIMAGE_INFO ABI");
struct Metadata {
    DWORD width, height, usage, autoFilter;
    Info source;
    HRESULT result;
};
struct Header { DWORD magic, version, bytes; Digest key, payload; };
static_assert(sizeof(Header) == 76, "texture cache header has no padding");

class Hash {
public:
    Hash() {
        valid_ = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void Add(const void* bytes, std::size_t size) {
        if (!valid_) return;
        if (size > ULONG_MAX || (size && BCryptHashData(hash_,
            static_cast<PUCHAR>(const_cast<void*>(bytes)), static_cast<ULONG>(size), 0) < 0)) valid_ = false;
    }
    template<class T> void Add(const T& value) { Add(&value, sizeof value); }
    bool Finish(Digest& output) { return valid_ && BCryptFinishHash(hash_, output.data(), 32, 0) >= 0; }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool valid_ = false;
};

struct Source {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    Digest digest{};
    ~Source() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    bool Open(LPCWSTR input, Bytes* encoded = nullptr, std::uint64_t limit = 128ull*1024*1024) {
        // GUI loading already has a deep legacy call stack. Keep path and
        // hashing buffers off that stack, including when this function inlines.
        std::vector<wchar_t> absolute(32768);
        const DWORD n = GetFullPathNameW(input, 32768, absolute.data(), nullptr);
        if (!n || n >= 32768) return false;
        path.assign(absolute.data(), n);
        // A stable read snapshot: an existing writer or delete/replace operation
        // makes this attempt fail, in which case the original loader runs.
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
            length.QuadPart > 128ll * 1024 * 1024 ||
            static_cast<std::uint64_t>(length.QuadPart) > limit) return false;
        if (encoded) encoded->resize(static_cast<std::size_t>(length.QuadPart));
        Hash hash;
        Bytes block(65536);
        std::uint64_t total = 0;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &read, nullptr)) return false;
            if (!read) break;
            total += read;
            if (total > static_cast<std::uint64_t>(length.QuadPart)) return false;
            hash.Add(block.data(), read);
            if (encoded) std::memcpy(encoded->data()+total-read, block.data(), read);
        }
        return total == static_cast<std::uint64_t>(length.QuadPart) && hash.Finish(digest);
    }
};

struct Prepared {
    Source source;
    Bytes encoded, dds, diskPayload;
    Info info{};
    Digest key{};
    bool ready = false, queued = false;
};
struct BatchState {
    d3dx::FnCreateTexFromFileExW original = nullptr;
    d3dx::FnCreateTexInMemEx fromMemory = nullptr;
    IDirect3DDevice9* device = nullptr; // accessed only on the constructing thread
    std::vector<std::unique_ptr<Prepared>> items;
    std::mutex mutex;
    std::condition_variable condition;
    std::array<std::thread,2> workers;
    bool stopping = false;
    unsigned workerCount = 0;
    std::uint64_t reservedBytes = 0;
    BatchState* previous = nullptr;
    ~BatchState() {
        { std::lock_guard<std::mutex> lock(mutex); stopping=true; }
        condition.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    void Work() noexcept {
        for (;;) {
            Prepared* item = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,[&] {
                    if (stopping) return true;
                    for (auto& p:items) if (p->queued) return true;
                    return false;
                });
                if (stopping) return;
                for (auto& p:items) if (p->queued) { item=p.get(); p->queued=false; break; }
            }
            bool ok = false;
            try { ok=texture_cpu_decode::Decode(item->encoded,item->info.width,item->info.height,item->dds); }
            catch (...) {}
            Bytes{}.swap(item->encoded);
            if (ok) ++decoded; else ++prepareFailures;
            { std::lock_guard<std::mutex> lock(mutex); item->ready=true; }
            condition.notify_all();
        }
    }
};
thread_local BatchState* currentBatch = nullptr;

Prepared* FindPrepared(d3dx::FnCreateTexFromFileExW original, IDirect3DDevice9* device, LPCWSTR file) {
    // MMHack can unwrap the host's device before calling its native D3DX
    // import. CPU pixels depend on source/decoder, not wrapper pointer identity;
    // Native rebuilds and validates the actual device's disk key below.
    (void)device;
    if (!currentBatch || currentBatch->original!=original) return nullptr;
    std::vector<wchar_t> absolute(32768);
    const auto n=GetFullPathNameW(file,32768,absolute.data(),nullptr);
    if (!n || n>=32768) return nullptr;
    for (auto& item:currentBatch->items) if (item->source.path==absolute.data()) return item.get();
    return nullptr;
}
bool CreatePrepared(Prepared* item, IDirect3DDevice9* device, DWORD usage,
                    Info& info, IDirect3DTexture9** output, HRESULT& result) {
    if (!item || !currentBatch || !currentBatch->fromMemory) return false;
    {
        std::unique_lock<std::mutex> lock(currentBatch->mutex);
        currentBatch->condition.wait(lock,[&] {return item->ready;});
    }
    if (item->dds.empty()) return false;
    ComPtr<IDirect3DTexture9> texture;
    result=currentBatch->fromMemory(device,item->dds.data(),static_cast<UINT>(item->dds.size()),
        UINT_MAX,UINT_MAX,1,usage,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,UINT_MAX,UINT_MAX,0,
        nullptr,nullptr,&texture);
    if (FAILED(result) || !texture) return false;
    info=item->info; *output=texture.Detach(); ++preparedUses;
    runtime_log::WritePath("TEXTURE_PREPARE parallel",item->source.path.c_str());
    return true;
}

bool Disabled() {
    if(!cache_manager::Enabled())return true;
    // LoadTextureShared already retains the live GPU texture. In RAM-only
    // mode keep that pool and avoid an additional decoded-pixel copy on disk.
    if (cache_storage_policy::MemoryOnly()) return true;
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE", value, 8) == 1 && value[0] == L'1';
}
fs::path Root() {
    std::vector<wchar_t> value(32768);
    DWORD count = GetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR", value.data(), 32768);
    if (count && count < 32768) return fs::path(std::wstring(value.data(), count));
    count = GetModuleFileNameW(nullptr, value.data(), 32768);
    if (!count || count >= 32768) return {};
    const auto root=cache_manager::Root();return root.empty()?fs::path{}:root/L"Textures";
}
fs::path Entry(const fs::path& root, const Source& source, const Digest& key) {
    if (root.empty()) return {};
    std::wstring group = fs::path(source.path).stem().wstring();
    for (auto& c : group) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    if (group.size() > 64) {
        group.resize(64);
        if (group.back() >= 0xd800 && group.back() <= 0xdbff) group.pop_back();
    }
    while (!group.empty() && (group.back() == L'.' || group.back() == L' ')) group.pop_back();
    if (group.empty()) group = L"texture";
    // Prefix reserved Win32 device stems, including names with another suffix.
    auto token = group.substr(0, group.find(L'.'));
    if (!_wcsicmp(token.c_str(), L"CON") || !_wcsicmp(token.c_str(), L"PRN") ||
        !_wcsicmp(token.c_str(), L"AUX") || !_wcsicmp(token.c_str(), L"NUL") ||
        (token.size() == 4 && token[3] >= L'1' && token[3] <= L'9' &&
         (!_wcsnicmp(token.c_str(), L"COM", 3) || !_wcsnicmp(token.c_str(), L"LPT", 3)))) group.insert(0, 1, L'_');
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name;
    for (auto b : key) { name += digits[b >> 4]; name += digits[b & 15]; }
    return root / group / (name + L".mtc");
}

bool DecoderIdentity(d3dx::FnCreateTexFromFileExW original, Digest& identity) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(original), &module) ||
        GetProcAddress(module, "D3DXCreateTextureFromFileExW") != reinterpret_cast<FARPROC>(original)) return false;
    effect_compile_cache::D3DX api;
    if (!effect_compile_cache::ResolveD3DX(reinterpret_cast<void*>(
            GetProcAddress(module, "D3DXCreateEffectFromFileW")), api)) return false;
    identity = api.identity;
    return true;
}
bool BuildKey(IDirect3DDevice9* device, const Source& source, const Digest& decoder,
              DWORD usage, Digest& key) {
    D3DCAPS9 caps{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    D3DADAPTER_IDENTIFIER9 adapter{};
    ComPtr<IDirect3D9> d3d;
    if (FAILED(device->GetDeviceCaps(&caps)) || FAILED(device->GetCreationParameters(&creation)) ||
        FAILED(device->GetDirect3D(&d3d)) ||
        FAILED(d3d->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &adapter))) return false;
    Hash hash;
    hash.Add(kVersion); hash.Add(decoder); hash.Add(source.digest);
    const auto pathBytes = static_cast<DWORD>(source.path.size() * sizeof(wchar_t));
    hash.Add(pathBytes); hash.Add(source.path.data(), pathBytes);
    hash.Add(usage); hash.Add(caps); hash.Add(creation.DeviceType); hash.Add(creation.BehaviorFlags);
    hash.Add(adapter.DriverVersion); hash.Add(adapter.DeviceIdentifier);
    return hash.Finish(key);
}

bool Read(const fs::path& path, const Digest& key, Bytes& payload,
          std::uint64_t limit = sizeof(Metadata)+kMaxPixels) {
    if (path.empty()) return false;
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if(error)return false;
    struct Validation {
        const fs::path& path;
        bool valid=false;
        ~Validation(){if(!valid)cache_manager::Invalidate(path);}
    } validation{path};
    if (size < sizeof(Header) + sizeof(Metadata) ||
        size > sizeof(Header) + sizeof(Metadata) + kMaxPixels) return false;
    if(size-sizeof(Header)>limit){validation.valid=true;return false;}
    std::ifstream stream(path, std::ios::binary);
    Header header{};
    if (!stream.read(reinterpret_cast<char*>(&header), sizeof header) || header.magic != kMagic ||
        header.version != kVersion || header.key != key || header.bytes != size - sizeof header) return false;
    payload.resize(header.bytes);
    if (!stream.read(reinterpret_cast<char*>(payload.data()), payload.size()) || stream.peek() != EOF) return false;
    Hash hash; hash.Add(payload.data(), payload.size());
    Digest actual{};
    const bool valid=hash.Finish(actual) && actual == header.payload;
    validation.valid=valid;
    if(valid)cache_manager::Touch(path);
    return valid;
}
bool Budget(const fs::path& root, const fs::path& target, std::uint64_t incoming) {
    if (incoming > kMaxDisk) return false;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
    if (error) return false;
    std::uint64_t bytes = incoming;
    unsigned count = 1, visited = 0;
    for (; it != end; it.increment(error)) {
        if (error || ++visited > 4096) return false;
        if (it->is_directory(error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            if (it.depth() >= 1) it.disable_recursion_pending();
            continue;
        }
        if (error) return false;
        if (it->path().extension() != L".mtc" || it->path() == target) continue;
        bytes += it->file_size(error); ++count;
        if (error || bytes > kMaxDisk || count > kMaxEntries) return false;
    }
    return !error && bytes <= kMaxDisk && count <= kMaxEntries;
}
bool Write(const fs::path& root, const fs::path& path, const Digest& key, const Bytes& payload) {
    if (path.empty()) return false;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    cache_manager::WriteReservation reservation(path,sizeof(Header)+payload.size());
    if (error || !reservation || (!cache_manager::Managed(path) && !Budget(root, path, sizeof(Header) + payload.size()))) return false;
    Header header{kMagic, kVersion, static_cast<DWORD>(payload.size()), key, {}};
    Hash hash; hash.Add(payload.data(), payload.size());
    if (!hash.Finish(header.payload)) return false;
    const fs::path temp = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(serial.fetch_add(1));
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool good = WriteFile(file, &header, sizeof header, &written, nullptr) && written == sizeof header &&
        WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) && written == payload.size() &&
        FlushFileBuffers(file);
    CloseHandle(file);
    good = good && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!good) DeleteFileW(temp.c_str());
    if(good)reservation.Commit();
    return good;
}

bool Capture(IDirect3DTexture9* texture, const Info& source, HRESULT result, DWORD usage, Bytes& output) {
    D3DSURFACE_DESC desc{};
    // Hand-authored source mip chains are not reconstructed from level zero.
    if (source.depth != 1 || source.resourceType != D3DRTYPE_TEXTURE ||
        (usage && source.mipLevels > 1) ||
        !texture || texture->GetLevelCount() != 1 || FAILED(texture->GetLevelDesc(0, &desc)) ||
        desc.Type != D3DRTYPE_SURFACE || desc.Format != D3DFMT_A8R8G8B8 ||
        desc.Pool != D3DPOOL_MANAGED || desc.Usage != usage || !desc.Width || !desc.Height ||
        desc.Width > 16384 || desc.Height > 16384) return false;
    const std::uint64_t bytes = std::uint64_t(desc.Width) * desc.Height * 4;
    if (bytes > kMaxPixels) return false;
    const Metadata meta{desc.Width, desc.Height, desc.Usage,
        static_cast<DWORD>(texture->GetAutoGenFilterType()), source, result};
    output.resize(sizeof meta + static_cast<std::size_t>(bytes));
    std::memcpy(output.data(), &meta, sizeof meta);
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY))) return false;
    const bool good = locked.pBits && locked.Pitch >= static_cast<int>(desc.Width * 4);
    if (good) for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(output.data() + sizeof meta + std::size_t(y) * desc.Width * 4,
            static_cast<const unsigned char*>(locked.pBits) + std::size_t(y) * locked.Pitch, desc.Width * 4);
    const HRESULT unlock = texture->UnlockRect(0);
    return good && SUCCEEDED(unlock);
}
bool Restore(IDirect3DDevice9* device, const Bytes& payload, DWORD usage,
             Info& info, IDirect3DTexture9** output, HRESULT& result) {
    if (payload.size() < sizeof(Metadata)) return false;
    Metadata meta{}; std::memcpy(&meta, payload.data(), sizeof meta);
    const auto bytes = std::uint64_t(meta.width) * meta.height * 4;
    if (!meta.width || !meta.height || meta.width > 16384 || meta.height > 16384 ||
        bytes > kMaxPixels || sizeof meta + bytes != payload.size() || meta.usage != usage ||
        !meta.source.width || !meta.source.height || meta.source.depth != 1 ||
        meta.source.resourceType != D3DRTYPE_TEXTURE || (usage && meta.source.mipLevels > 1) || FAILED(meta.result) ||
        (usage && meta.autoFilter != D3DTEXF_POINT && meta.autoFilter != D3DTEXF_LINEAR)) return false;
    ComPtr<IDirect3DTexture9> texture;
    if (FAILED(device->CreateTexture(meta.width, meta.height, 1, usage, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &texture, nullptr))) return false;
    if (usage && FAILED(texture->SetAutoGenFilterType(static_cast<D3DTEXTUREFILTERTYPE>(meta.autoFilter)))) return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) return false;
    const bool good = locked.pBits && locked.Pitch >= static_cast<int>(meta.width * 4);
    if (good) for (UINT y = 0; y < meta.height; ++y)
        std::memcpy(static_cast<unsigned char*>(locked.pBits) + std::size_t(y) * locked.Pitch,
            payload.data() + sizeof meta + std::size_t(y) * meta.width * 4, meta.width * 4);
    const auto unlocked = texture->UnlockRect(0);
    if (!good || FAILED(unlocked)) return false;
    if (usage) texture->GenerateMipSubLevels();
    info = meta.source; result = meta.result; *output = texture.Detach();
    return true;
}

HRESULT Native(d3dx::FnCreateTexFromFileExW original, IDirect3DDevice9* device,
    LPCWSTR file, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, DWORD filter, DWORD mipFilter, D3DCOLOR color, void* info,
    void* palette, IDirect3DTexture9** output) {
    const DWORD incoming = GetLastError();
    HRESULT originalResult = E_FAIL;
    bool called = false;
    auto fallback = [&] {
        SetLastError(incoming); called = true; ++nativeCalls;
        originalResult = original(device, file, width, height, levels, usage, format,
            pool, filter, mipFilter, color, info, palette, output);
        return originalResult;
    };
    if (!insideHostLoad || !device || !file || !*file || !output || palette ||
        width != UINT_MAX || height != UINT_MAX || levels != 1 ||
        (usage != 0 && usage != D3DUSAGE_AUTOGENMIPMAP) || format != D3DFMT_A8R8G8B8 ||
        pool != D3DPOOL_MANAGED || filter != UINT_MAX || mipFilter != UINT_MAX || color != 0) return fallback();
    try {
        auto* prepared=FindPrepared(original,device,file);
        const bool diskAllowed=!Disabled();
        if (!diskAllowed && !prepared) return fallback();
        Digest decoder{}, key{};
        Source localSource;
        Source& source=prepared ? prepared->source : localSource;
        if ((!prepared && !source.Open(file)) || !DecoderIdentity(original, decoder) ||
            !BuildKey(device, source, decoder, usage, key)) return fallback();
        const auto root = Root(), entry = Entry(root, source, key);
        Bytes payload;
        Info restored{};
        HRESULT result = E_FAIL;
        const Bytes* readyPayload=prepared && prepared->key==key && !prepared->diskPayload.empty()
            ? &prepared->diskPayload : nullptr;
        if (diskAllowed && ((readyPayload && Restore(device,*readyPayload,usage,restored,output,result)) ||
            (!readyPayload && Read(entry,key,payload) && Restore(device,payload,usage,restored,output,result)))) {
            if (info) std::memcpy(info, &restored, sizeof restored);
            ++hits; runtime_log::WritePath("TEXTURE_PREPARE hit", file);
            cache_manager::Touch(entry);
            SetLastError(incoming); return result;
        }
        // Always return the original object's HRESULT/ownership on a miss.
        // Cache extraction/storage after success is strictly best effort.
        Info captured{};
        SetLastError(incoming);
        if (CreatePrepared(prepared,device,usage,captured,output,originalResult)) {
            called=true;
            if (info) std::memcpy(info,&captured,sizeof captured);
        } else {
            called = true; ++nativeCalls;
            originalResult = original(device, file, width, height, levels, usage, format,
                pool, filter, mipFilter, color, info ? info : &captured, palette, output);
        }
        const DWORD after = GetLastError();
        if (SUCCEEDED(originalResult) && *output) {
            try {
                if (info) std::memcpy(&captured, info, sizeof captured);
                if (diskAllowed && Capture(*output, captured, originalResult, usage, payload) && Write(root, entry, key, payload)) {
                    ++stores; runtime_log::WritePath("TEXTURE_PREPARE store", file);
                }
            } catch (...) { runtime_log::Write("TEXTURE_PREPARE store skipped"); }
        }
        SetLastError(after); return originalResult;
    } catch (...) {
        if (called) return originalResult;
        return fallback();
    }
}

template<bool V43> HRESULT WINAPI Inner(IDirect3DDevice9* device, LPCWSTR file,
    UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    DWORD filter, DWORD mipFilter, D3DCOLOR color, void* info, void* palette, IDirect3DTexture9** output) {
    const auto original = (V43 ? original43 : original32).load();
    return original ? Native(original, device, file, w, h, levels, usage, format, pool,
        filter, mipFilter, color, info, palette, output) : E_NOINTERFACE;
}

void Install() {
    const auto module = GetModuleHandleW(L"MMHack.dll");
    if (!module) return;
    std::lock_guard<std::mutex> lock(installMutex);
    if (patchedModule == module) return;
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= size || imports.Size > size - imports.VirtualAddress) return;
    const auto* list = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (std::size_t i = 0; i < imports.Size / sizeof(*list) && list[i].Name; ++i) {
        const auto& item = list[i];
        if (item.Name >= size || strnlen_s(reinterpret_cast<const char*>(base + item.Name), size - item.Name) == size - item.Name) continue;
        const char* library = reinterpret_cast<const char*>(base + item.Name);
        const bool v43 = !_stricmp(library, "d3dx9_43.dll");
        if (!v43 && _stricmp(library, "d3dx9_32.dll")) continue;
        if (!item.OriginalFirstThunk || item.OriginalFirstThunk >= size || !item.FirstThunk || item.FirstThunk >= size) continue;
        const auto original = reinterpret_cast<d3dx::FnCreateTexFromFileExW>(
            GetProcAddress(GetModuleHandleA(library), "D3DXCreateTextureFromFileExW"));
        if (!original) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + item.OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + item.FirstThunk);
        for (std::size_t j = 0; j < (size - item.OriginalFirstThunk) / sizeof(*names) &&
             j < (size - item.FirstThunk) / sizeof(*slots) && names[j].u1.AddressOfData; ++j) {
            if (IMAGE_SNAP_BY_ORDINAL(names[j].u1.Ordinal)) continue;
            const auto rva = names[j].u1.AddressOfData;
            if (rva >= size || size - rva <= sizeof(WORD)) continue;
            const auto* name = reinterpret_cast<const char*>(base + rva + sizeof(WORD));
            if (strnlen_s(name, size - rva - sizeof(WORD)) == size - rva - sizeof(WORD) ||
                std::strcmp(name, "D3DXCreateTextureFromFileExW") ||
                slots[j].u1.Function != reinterpret_cast<ULONG_PTR>(original)) continue;
            auto* slot = &slots[j].u1.Function;
            DWORD protection = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) continue;
            (v43 ? original43 : original32).store(original);
            InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                reinterpret_cast<void*>(v43 ? &Inner<true> : &Inner<false>), reinterpret_cast<void*>(original));
            DWORD ignored = 0; VirtualProtect(slot, sizeof(void*), protection, &ignored);
        }
    }
    patchedModule = module;
}
} // namespace

struct Batch::Impl : BatchState {};

Batch::Batch(d3dx::FnCreateTexFromFileExW outer, IDirect3DDevice9* device,
             const std::vector<std::wstring>& paths) noexcept {
    const DWORD incoming=GetLastError();
    struct LastError { DWORD value; ~LastError(){SetLastError(value);} } lastError{incoming};
    try {
        if (!device || !outer || paths.empty() || parallel_prepare_policy::Disabled()) return;
        wchar_t disabled[8]{};
        if (GetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE",disabled,8)==1 && disabled[0]==L'1') return;
        Install();
        auto state=std::make_unique<Impl>();
        state->device=device;
        HMODULE module=nullptr;
        for (const char* name:{"d3dx9_32.dll","d3dx9_43.dll"}) {
            const auto candidate=GetModuleHandleA(name);
            if (candidate && GetProcAddress(candidate,"D3DXCreateTextureFromFileExW")==reinterpret_cast<FARPROC>(outer)) {
                module=candidate; state->original=outer; break;
            }
        }
        // A mixed-version or unknown proxy is left alone. The ordinary Create
        // path can still use its actual inner D3DX version's persistent cache.
        if (!module) {
            const auto a=original32.load(),b=original43.load();
            if (bool(a)==bool(b)) return;
            state->original=a ? a : b;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(state->original),&module);
        }
        using GetInfo=HRESULT(WINAPI*)(LPCVOID,UINT,void*);
        const auto getInfo=reinterpret_cast<GetInfo>(GetProcAddress(module,"D3DXGetImageInfoFromFileInMemory"));
        state->fromMemory=reinterpret_cast<d3dx::FnCreateTexInMemEx>(GetProcAddress(module,"D3DXCreateTextureFromFileInMemoryEx"));
        Digest decoder{};
        if (!getInfo || !state->fromMemory || !DecoderIdentity(state->original,decoder)) return;
        constexpr std::uint64_t budget=128ull*1024*1024;
        for (const auto& path:paths) {
            if (state->items.size()>=64 || state->reservedBytes>=budget) break;
            std::vector<wchar_t> absolute(32768);
            const auto n=GetFullPathNameW(path.c_str(),32768,absolute.data(),nullptr);
            if (!n || n>=32768) continue;
            bool duplicate=false;
            for (auto& previous:state->items) if (previous->source.path==absolute.data()) {duplicate=true;break;}
            if (duplicate) continue;
            auto item=std::make_unique<Prepared>();
            if (!item->source.Open(path.c_str(),&item->encoded,budget-state->reservedBytes)) continue;
            const bool diskAllowed=!Disabled();
            if (!BuildKey(device,item->source,decoder,D3DUSAGE_AUTOGENMIPMAP,item->key)) continue;
            const auto available=budget-state->reservedBytes;
            // Warm persistent entries already contain exact resized pixels;
            // queueing a fresh decoder here would repeat work needlessly.
            if (diskAllowed && Read(Entry(Root(),item->source,item->key),item->key,item->diskPayload,
                                    available-item->encoded.size())) {
                state->reservedBytes+=item->diskPayload.size();
                Bytes{}.swap(item->encoded); item->ready=true; ++diskReady;
            } else {
                Bytes{}.swap(item->diskPayload);
                const bool supported=texture_cpu_decode::Supported(item->encoded) &&
                    SUCCEEDED(getInfo(item->encoded.data(),static_cast<UINT>(item->encoded.size()),&item->info)) &&
                    item->info.width && item->info.height && item->info.width<=16384 && item->info.height<=16384 &&
                    item->info.depth==1 && item->info.mipLevels==1 && item->info.resourceType==D3DRTYPE_TEXTURE;
                const auto needed=std::uint64_t(item->info.width)*item->info.height*4+128+item->encoded.size();
                if (!supported || needed>available) {
                    // Reuse the already validated source/hash even when its
                    // pixels must be decoded by the original file loader.
                    Bytes{}.swap(item->encoded); item->ready=true;
                } else {
                if (!state->workerCount) {
                    for (unsigned i=0;i<2;++i) {
                        try { state->workers[i]=std::thread([p=state.get()] {p->Work();}); ++state->workerCount; }
                        catch (...) {break;}
                    }
                    if (!state->workerCount) break;
                }
                state->reservedBytes+=needed; item->queued=true;
                }
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->items.push_back(std::move(item));
            }
            ++staged; state->condition.notify_one();
        }
        runtime_log::Write("TEXTURE_PREPARE batch workers=%u entries=%zu reserved=%llu",
            state->workerCount,state->items.size(),static_cast<unsigned long long>(state->reservedBytes));
        state->previous=currentBatch; currentBatch=state.get(); impl_=std::move(state);
    } catch (...) { ++prepareFailures; }
}
Batch::~Batch() { if (impl_) currentBatch=impl_->previous; }

ParallelStatistics GetParallelStatistics() noexcept {
    return {staged.load(),decoded.load(),preparedUses.load(),diskReady.load(),prepareFailures.load()};
}

HRESULT Create(d3dx::FnCreateTexFromFileExW outer, IDirect3DDevice9* device,
    LPCWSTR path, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, DWORD filter, DWORD mipFilter, D3DCOLOR color, void* info,
    void* palette, IDirect3DTexture9** output) {
    if (!outer) return E_NOINTERFACE;
    const DWORD incoming = GetLastError();
    try { Install(); } catch (...) {}
    const bool previous = insideHostLoad; insideHostLoad = true;
    struct Scope { bool previous; ~Scope() { insideHostLoad = previous; } } scope{previous};
    SetLastError(incoming);
    // A real D3DX export goes directly through Native. Proxy imports take the
    // same fallback to their original outer wrapper, then our patched inner
    // MMHack import performs the cache work before registration completes.
    for (const char* name : {"d3dx9_32.dll", "d3dx9_43.dll"}) {
        const auto module = GetModuleHandleA(name);
        if (module && GetProcAddress(module, "D3DXCreateTextureFromFileExW") == reinterpret_cast<FARPROC>(outer)) {
            SetLastError(incoming);
            return Native(outer, device, path, width, height, levels, usage, format,
                pool, filter, mipFilter, color, info, palette, output);
        }
    }
    SetLastError(incoming);
    return outer(device, path, width, height, levels, usage, format,
        pool, filter, mipFilter, color, info, palette, output);
}
Statistics GetStatistics() noexcept { return {hits.load(), nativeCalls.load(), stores.load()}; }
} // namespace mikumikustudio::texture_prepare_cache
