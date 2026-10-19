#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/owned_text.hpp"
// MAINTENANCE: regression-keep; malformed PMX text must fail before allocation
// or string conversion, and partially constructed models must remain disposable.
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <new>
#include <vector>
namespace { thread_local std::size_t failAllocationSize = 0; }
void* operator new(std::size_t size) {
    if (failAllocationSize && size == failAllocationSize) throw std::bad_alloc();
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
using namespace mikumikustudio;
namespace {
using Bytes = std::vector<unsigned char>;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
template<class T> void Put(Bytes& bytes, T value) {
    const auto* first = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof value);
}
void Text(Bytes& bytes, const wchar_t* value = L"A") {
    const auto size = static_cast<unsigned>(wcslen(value) * sizeof(wchar_t));
    Put(bytes, size);
    const auto* first = reinterpret_cast<const unsigned char*>(value);
    bytes.insert(bytes.end(), first, first + size);
}
Bytes Header(int names = 4) {
    Bytes bytes{'P','M','X',' '}; Put(bytes, 2.f);
    for (unsigned char v : {8,0,0,1,1,1,1,1,1}) Put(bytes, v);
    for (int i = 0; i < names; ++i) Text(bytes);
    return bytes;
}
Bytes Geometry() {
    auto bytes = Header(); Put(bytes, 0u); Put(bytes, 0u); return bytes;
}
Bytes Bones() {
    auto bytes = Geometry(); Put(bytes, 0u); Put(bytes, 0u); Put(bytes, 1u);
    return bytes;
}
Bytes AfterBone(const wchar_t* name = L"A", const wchar_t* english = L"") {
    auto bytes = Bones(); Text(bytes, name); Text(bytes, english);
    for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
    Put(bytes, static_cast<unsigned char>(255)); Put(bytes, 0u);
    Put(bytes, static_cast<unsigned short>(0));
    for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
    return bytes;
}
void Reject(const Bytes& bytes, const wchar_t* boneName = nullptr) {
    const auto baseline = owned_text::Count();
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    Check(GetTempPathW(MAX_PATH, directory) != 0, "temp directory");
    Check(GetTempFileNameW(directory, L"pmx", 0, path) != 0, "temp name");
    int fd = -1;
    fd = _wopen(path, _O_BINARY | _O_WRONLY | _O_TRUNC);
    Check(fd >= 0, "open fixture");
    Check(_write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) == bytes.size(), "write fixture");
    _close(fd);
    fd = _wopen(path, _O_BINARY | _O_RDONLY);
    Check(fd >= 0, "open PMX");
    Check(_lseek(fd, 3, SEEK_SET) == 3, "dispatch consumes PMX magic");
    auto model = std::make_unique<mdl::ModelRecord>();
    PathResolutionWorkspace paths{};
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    Check(!LoadPMX(raw, nullptr, 0, 0, paths, fd), "reject malformed text");
    if (boneName) {
        Check(model->boneTable != nullptr, "valid preceding bone retained");
#if MMD_ORIGINAL_PROFILE
        Check(wcscmp(model->boneTable[0].jpText, boneName) == 0, "single code unit or empty-name fallback");
        Check(wcscmp(model->boneTable[0].enText, boneName) == 0, "empty English name uses Japanese fallback");
#else
        Check(!model->boneTable[0].jpText && !model->boneTable[0].enText,
            "rejected load releases preceding owned names and leaves disposable null fields");
#endif
    }
    Check(owned_text::Count() == baseline, "malformed PMX leaves no registered text");
    ModelDispose(raw);
    Check(owned_text::Count() == baseline, "disposing malformed PMX preserves registry baseline");
    Check(DeleteFileW(path) != FALSE, "loader closed rejected file");
}
}
void ExpandedDisplay() {
    const auto baseline = owned_text::Count();
    auto bytes=AfterBone(L"超过十四字符的完整骨骼名称_左侧_\U0001f31f");
    Put(bytes,1u);Text(bytes,L"完整的表情名称_\U0001f31f");Text(bytes,L"");
    Put(bytes,static_cast<unsigned char>(1));Put(bytes,static_cast<unsigned char>(1));Put(bytes,0u);
    Put(bytes,300u);
    for(unsigned i=0;i<300;++i){
        const auto name=L"显示分组_"+std::to_wstring(i);Text(bytes,name.c_str());Text(bytes,L"");
        Put(bytes,static_cast<unsigned char>(i<2));Put(bytes,3u);
        for(int j=0;j<2;++j){Put(bytes,static_cast<unsigned char>(1));Put(bytes,static_cast<unsigned char>(0));}
        Put(bytes,static_cast<unsigned char>(0));Put(bytes,static_cast<unsigned char>(0));
    }
    Put(bytes,0u);Put(bytes,0u);
    wchar_t directory[MAX_PATH]{},path[MAX_PATH]{};GetTempPathW(MAX_PATH,directory);GetTempFileNameW(directory,L"pmx",0,path);
    int fd=_wopen(path,_O_BINARY|_O_WRONLY|_O_TRUNC);Check(fd>=0,"open expanded PMX fixture");
    Check(_write(fd,bytes.data(),unsigned(bytes.size()))==bytes.size(),"write expanded PMX");_close(fd);
    fd=_wopen(path,_O_BINARY|_O_RDONLY);_lseek(fd,3,SEEK_SET);
    auto model=std::make_unique<mdl::ModelRecord>();PathResolutionWorkspace paths{};
    auto* raw=reinterpret_cast<unsigned char*>(model.get());
    Check(LoadPMX(raw,nullptr,0,0,paths,fd),"load expanded display PMX");
    Check(mdl::DisplayGroupCount(*model)>=300&&mdl::FacialFrameCount(*model)==600,"all groups and facial display items survive PMX load");
    Check(mdl::FrameTargetIndex(model->displayFrames[599])==0,"last facial item retains its morph index");
    Check(text_encoding::BoneName(model->boneTable[0],false)==L"超过十四字符的完整骨骼名称_左侧_\U0001f31f","PMX preserves complete Unicode bone identity");
#if !MMD_ORIGINAL_PROFILE
    Check(owned_text::Count() == baseline + 8, "display frame temporary text registrations are released after load");
#endif
    ModelDispose(raw);Check(DeleteFileW(path)!=FALSE,"expanded PMX is closed and disposable");
    Check(owned_text::Count() == baseline, "successful PMX disposal releases all text registrations");
}
void TextCapacityAndRetry() {
#if !MMD_ORIGINAL_PROFILE
    const auto baseline = owned_text::Count();
    auto open = [](const Bytes& bytes, wchar_t (&path)[MAX_PATH]) {
        wchar_t directory[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"pmx", 0, path), "create capacity fixture");
        int fd = _wopen(path, _O_BINARY | _O_WRONLY | _O_TRUNC);
        Check(fd >= 0 && _write(fd, bytes.data(), unsigned(bytes.size())) == bytes.size(), "write capacity fixture");
        _close(fd); fd = _wopen(path, _O_BINARY | _O_RDONLY);
        Check(fd >= 0 && _lseek(fd, 3, SEEK_SET) == 3, "open capacity fixture"); return fd;
    };
    auto rawText = [](Bytes& bytes, const wchar_t* value, unsigned units) {
        Put(bytes, units * unsigned(sizeof(wchar_t)));
        const auto* begin = reinterpret_cast<const unsigned char*>(value);
        bytes.insert(bytes.end(), begin, begin + units * sizeof(wchar_t));
    };
    {
        auto bytes = Header(0);
        wchar_t title[64]{}; title[0] = L'P'; title[63] = L'X'; rawText(bytes, title, 64);
        for (int i = 0; i < 3; ++i) Text(bytes, L"");
        Put(bytes, 0u); Put(bytes, 0u); // no vertices or indices
        Put(bytes, 1u); Text(bytes, L"unused-texture.png"); Put(bytes, 0u); // no materials
        Put(bytes, 1u); const wchar_t bone[] = {L'B', 0, L'x', L'y'};
        rawText(bytes, bone, 4); Text(bytes, L"");
        for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
        Put(bytes, static_cast<unsigned char>(255)); Put(bytes, 0u); Put(bytes, static_cast<unsigned short>(0));
        for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
        Put(bytes, 1u); Text(bytes, L""); Text(bytes, L"");
        Put(bytes, static_cast<unsigned char>(1)); Put(bytes, static_cast<unsigned char>(1)); Put(bytes, 0u);
        Put(bytes, 0u); Put(bytes, 0u); // no display frames or rigids
        wchar_t path[MAX_PATH]{}; const int fd = open(bytes, path);
        auto model = std::make_unique<mdl::ModelRecord>(); PathResolutionWorkspace paths{};
        auto* raw = reinterpret_cast<unsigned char*>(model.get());
        Check(LoadPMX(raw, nullptr, 0, 0, paths, fd), "load early NUL and generated PMX names");
        auto capacity = [](const wchar_t* pointer, std::size_t bytes) {
            owned_text::Allocation value;
            return owned_text::Find(pointer, value) && value.bytes == bytes;
        };
        Check(capacity(model->pmxTextBuffers[0], 130) && wcscmp(model->pmxTextBuffers[0], L"P") == 0,
            "PMX header tracks payload capacity beyond early NUL");
        for (int i = 1; i < 4; ++i) Check(capacity(model->pmxTextBuffers[i], 40), "generated header text retains fixed allocation");
        Check(capacity(model->boneTable[0].jpText, 10) && capacity(model->boneTable[0].enText, 4),
            "bone payload and fallback record distinct actual capacities");
        Check(capacity(model->morphs[0].jpText, 40) &&
            capacity(model->morphs[0].enText, (wcslen(model->morphs[0].jpText) + 1) * sizeof(wchar_t)),
            "empty morph placeholder and fallback record distinct capacities");
        Check(owned_text::Count() == baseline + 8, "temporary texture capacity is released after successful load");
        ModelDispose(raw); Check(DeleteFileW(path), "capacity fixture closes");
        Check(owned_text::Count() == baseline, "early NUL model disposal releases complete allocation registry");
    }
    {
        auto model = std::make_unique<mdl::ModelRecord>(); PathResolutionWorkspace paths{};
        auto* raw = reinterpret_cast<unsigned char*>(model.get());
        for (auto& pointer : model->pmxTextBuffers) { pointer = owned_text::AllocateBytes(40); wcscpy_s(pointer, 20, L"old"); }
        model->boneTable = static_cast<mdl::BoneRecord*>(std::calloc(1, sizeof(mdl::BoneRecord))); model->boneCount = 1;
        Check(model->boneTable != nullptr, "allocate prior retry table");
        auto* oldBones = model->boneTable;
        oldBones[0].jpText = owned_text::AllocateBytes(40); wcscpy_s(oldBones[0].jpText, 20, L"old bone");
        auto* oldTitle = model->pmxTextBuffers[0]; auto* oldBoneName = oldBones[0].jpText;
        for (int retry = 0; retry < 2; ++retry) {
            auto bytes = Header(2); Put(bytes, 1u);
            wchar_t path[MAX_PATH]{}; const int fd = open(bytes, path);
            Check(!LoadPMX(raw, nullptr, 0, 0, paths, fd), "repeated malformed header fails safely");
            Check(model->pmxTextBuffers[0] == oldTitle && model->boneTable == oldBones &&
                oldBones[0].jpText == oldBoneName && model->boneCount == 1 && owned_text::Count() == baseline + 5,
                "retry rollback preserves prior owning text and paired table bound");
            Check(DeleteFileW(path), "malformed retry file closes");
        }
        auto bytes = Geometry(); Put(bytes, 0u); Put(bytes, 0u); Put(bytes, 7u);
        wchar_t path[MAX_PATH]{}; const int fd = open(bytes, path);
        bool memory = false; failAllocationSize = sizeof(mdl::BoneRecord) * 7;
        try { (void)LoadPMX(raw, nullptr, 0, 0, paths, fd); } catch (const std::bad_alloc&) { memory = true; }
        failAllocationSize = 0;
        Check(memory && model->boneTable == oldBones && model->boneCount == 1 &&
            oldBones[0].jpText == oldBoneName && model->pmxTextBuffers[0] == oldTitle &&
            owned_text::Count() == baseline + 5, "failed table allocation restores prior count and releases only new text");
        Check(DeleteFileW(path), "exception closes PMX reader");
        ModelDispose(raw);
        Check(owned_text::Count() == baseline, "malformed and exceptional retries remain safely disposable");
    }
#endif
}
void OwnToonBoundaries() {
    const auto baseline = owned_text::Count();
    unsigned cases = 0;
    for (bool buffered : {false, true}) {
        fileio::bufferedReads = buffered;
        for (const unsigned width : {1u, 2u, 4u}) {
            const int maximum = width == 1 ? 127 : width == 2 ? 32767 : 0x7fffffff;
            for (const unsigned textures : {0u, 1u}) for (const int toon : {-1, 0, 1, maximum}) {
                auto bytes = Geometry(); bytes[12] = static_cast<unsigned char>(width);
                const auto index = [&](int value) {
                    if (width == 1) Put(bytes, static_cast<std::int8_t>(value));
                    else if (width == 2) Put(bytes, static_cast<std::int16_t>(value));
                    else Put(bytes, static_cast<std::int32_t>(value));
                };
                Put(bytes, textures); if (textures) Text(bytes, L"");
                Put(bytes, 1u); Text(bytes); Text(bytes);
                for (int i = 0; i < 11; ++i) Put(bytes, 0.f);
                Put(bytes, static_cast<unsigned char>(0));
                for (int i = 0; i < 5; ++i) Put(bytes, 0.f);
                index(-1); index(-1); Put(bytes, static_cast<unsigned char>(0));
                Put(bytes, static_cast<unsigned char>(0)); index(toon); Text(bytes, L""); Put(bytes, 0u);
                Put(bytes, 1u); Text(bytes); Text(bytes);
                for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
                Put(bytes, static_cast<unsigned char>(255)); Put(bytes, 0u);
                Put(bytes, static_cast<unsigned short>(0));
                for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
                for (int i = 0; i < 4; ++i) Put(bytes, 0u);
                wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
                Check(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"toon", 0, path),
                    "create own-toon fixture");
                int fd = _wopen(path, _O_BINARY | _O_WRONLY | _O_TRUNC);
                Check(fd >= 0 && _write(fd, bytes.data(), unsigned(bytes.size())) == bytes.size(),
                    "write own-toon fixture");
                _close(fd); fd = _wopen(path, _O_BINARY | _O_RDONLY);
                Check(fd >= 0 && _lseek(fd, 3, SEEK_SET) == 3, "open own-toon PMX");
                auto model = std::make_unique<mdl::ModelRecord>(); PathResolutionWorkspace paths{};
                auto* raw = reinterpret_cast<unsigned char*>(model.get());
                Check(LoadPMX(raw, nullptr, 0, 0, paths, fd),
                    "ordinary loader accepts absent, empty or out-of-range own toon without an invalid read");
                const auto* material = mdl::Materials(raw);
                Check(model->materialCount == 1 && material[0].toonReference == 0xFF &&
                    material[0].toonPath[0] == L'\0', "missing own toon clears its path and reference");
                Check(model->boneCount == 1 && text_encoding::BoneName(model->boneTable[0], false) == L"A",
                    "each texture index width leaves the following bone data aligned");
                ModelDispose(raw); Check(DeleteFileW(path) != FALSE, "own-toon fixture remains disposable");
                Check(owned_text::Count() == baseline, "own-toon load/disposal preserves text ownership");
                ++cases;
            }
        }
    }
    std::printf("PASS %u own-toon boundary cases, buffered and unbuffered, 1/2/4-byte indices\n", cases);
}
int main() {
    ExpandedDisplay();
    TextCapacityAndRetry();
    OwnToonBoundaries();
    unsigned cases = 0;
    for (bool buffered : {false, true}) {
        fileio::bufferedReads = buffered;
        for (int name = 0; name < 4; ++name) {
            for (unsigned prefix : {1u, 3u, 2u, 0x80000000u, 0xFFFFFFFEu}) {
                auto bytes = Header(name); Put(bytes, prefix); Reject(bytes); ++cases;
            }
            for (int available = 0; available < 4; ++available) {
                auto bytes = Header(name); bytes.insert(bytes.end(), available, 0);
                Reject(bytes); ++cases;
            }
        }
        // Failure in the first entry must not free uninitialised later pointers.
        for (int texture = 0; texture < 3; ++texture) {
            auto bytes = Geometry(); Put(bytes, 3u);
            for (int i = 0; i < texture; ++i) Text(bytes);
            Put(bytes, 1u); Reject(bytes); ++cases;
        }
        for (int language = 0; language < 2; ++language) {
            auto material = Geometry(); Put(material, 0u); Put(material, 1u);
            if (language) Text(material);
            Put(material, 2u); Reject(material); ++cases;
            auto bone = Bones(); if (language) Text(bone);
            Put(bone, 1u); Reject(bone); ++cases;
            auto morph = AfterBone(); Put(morph, 1u); if (language) Text(morph);
            Put(morph, 2u); Reject(morph); ++cases;
            auto display = AfterBone(); Put(display, 0u); Put(display, 1u);
            if (language) Text(display);
            Put(display, 1u); Reject(display); ++cases;
        }
        for (const auto* name : {L"A", L""}) {
            auto bytes = AfterBone(name); Put(bytes, 1u); Put(bytes, 1u);
            Reject(bytes, *name ? L"A" : L"Null_00"); ++cases;
        }
    }
    std::printf("PASS %u malformed PMX text cases, buffered and unbuffered\n", cases);
}
