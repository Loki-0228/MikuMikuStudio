#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/text_encoding.hpp"
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
#include <vector>
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
        Check(wcscmp(model->boneTable[0].jpText, boneName) == 0, "single code unit or empty-name fallback");
        Check(wcscmp(model->boneTable[0].enText, boneName) == 0, "empty English name uses Japanese fallback");
    }
    ModelDispose(raw);
    Check(DeleteFileW(path) != FALSE, "loader closed rejected file");
}
}
void ExpandedDisplay() {
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
    ModelDispose(raw);Check(DeleteFileW(path)!=FALSE,"expanded PMX is closed and disposable");
}
void OwnToonBoundaries() {
    for (const unsigned textures : {0u, 1u}) for (const unsigned char toon : {0, 1, 127, 255}) {
        auto bytes = Geometry();
        Put(bytes, textures);
        if (textures) Text(bytes, L"");
        Put(bytes, 1u); Text(bytes); Text(bytes);
        for (int i = 0; i < 11; ++i) Put(bytes, 0.0f);
        Put(bytes, static_cast<unsigned char>(0));
        for (int i = 0; i < 5; ++i) Put(bytes, 0.0f);
        for (const unsigned char value : {255, 255, 0, 0}) Put(bytes, value);
        Put(bytes, toon); Text(bytes, L""); Put(bytes, 0u);
        Put(bytes, 1u); Text(bytes); Text(bytes);
        for (int i = 0; i < 3; ++i) Put(bytes, 0.0f);
        Put(bytes, static_cast<unsigned char>(255)); Put(bytes, 0u);
        Put(bytes, static_cast<unsigned short>(0));
        for (int i = 0; i < 3; ++i) Put(bytes, 0.0f);
        for (int i = 0; i < 4; ++i) Put(bytes, 0u);
        wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH,directory) && GetTempFileNameW(directory,L"toon",0,path),"create own-toon fixture");
        int fd = _wopen(path,_O_BINARY|_O_WRONLY|_O_TRUNC);
        Check(fd >= 0 && _write(fd,bytes.data(),unsigned(bytes.size())) == bytes.size(),"write own-toon fixture");
        _close(fd); fd = _wopen(path,_O_BINARY|_O_RDONLY); _lseek(fd,3,SEEK_SET);
        auto model = std::make_unique<mdl::ModelRecord>(); PathResolutionWorkspace paths{};
        auto* raw = reinterpret_cast<unsigned char*>(model.get());
        Check(LoadPMX(raw,nullptr,0,0,paths,fd),"load model with absent, empty or out-of-range own toon");
        const auto* material = mdl::Materials(raw);
        Check(model->materialCount == 1 && material[0].toonReference == 0xFF &&
              material[0].toonPath[0] == L'\0',
              "missing own toon clears its path without indexing beyond the texture table");
        ModelDispose(raw); Check(DeleteFileW(path) != FALSE,"own-toon fixture remains disposable");
    }
}
int main() {
    ExpandedDisplay();
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
