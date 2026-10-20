#include "../src/render/model_toon.hpp"
#include "mikumikustudio/text_encoding.hpp"
namespace {
void NativeToonResolutionChecks() {
    ImportTextureFixture fixture;
    auto app = std::make_unique<MMDApp>(); auto model = std::make_unique<mdl::ModelRecord>();
    mdl::ModelMaterialRecord material{};
    unsigned used = 0;
    const auto add = [&](const wchar_t* path, IUnknown* texture) {
        auto& entry = fixture.renderer->resourcePool[used++]; const auto bytes = (std::wcslen(path) + 1) * sizeof(wchar_t);
        entry.heapBuffer = std::malloc(bytes); Check(entry.heapBuffer != nullptr, "prepare a distinct resolver cache slot");
        std::memcpy(entry.heapBuffer, path, bytes); entry.comObject = texture; InvalidateTextureLookup();
    };
    auto* neutral = new ImportTestTexture(fixture.state); add(L"neutral", neutral);
    auto* shared = new ImportTestTexture(fixture.state); add(L"shared", shared);
    auto* custom = new ImportTestTexture(fixture.state); add(L"D:\\model\\custom.bmp", custom);
    auto* own = new ImportTestTexture(fixture.state); add(L"D:\\model\\pmx-own.bmp", own);
    fixture.renderer->resourcePool[2].tag = 0x00332211;
    fixture.renderer->resourcePool[3].tag = 0x00ccbbaa;
    app->ToonTexture(0) = neutral; app->ToonTexture(2) = shared;
    app->state.toonEdgeTable[3] = .125f; app->state.toonEdgeTable[4] = .25f; app->state.toonEdgeTable[5] = .5f;
    std::wcscpy(model->modelDirectory, L"D:\\model\\");
    const auto resolve = [&]() { return ResolveModelToonTexture(app.get(), fixture.renderer.get(), *model, material); };
    const auto color = [&](float r, float g, float b) {
        float value[4]{}; ResolveModelToonColor(app.get(), fixture.renderer.get(), *model, material, value);
        return value[0] == r && value[1] == g && value[2] == b && value[3] == 1;
    };
    material.toonReference = 0xff;
    Check(resolve() == neutral, "unassigned PMD and PMX toons use the neutral shared texture");
    Check(color(1, 1, 1), "unassigned toon keeps the neutral shader tint");
    material.toonReference = 0; std::strcpy(model->pmdToonFileNames[0], "toon02.bmp");
    for (unsigned mode : {0u, 2u}) {
        model->physicsMode = static_cast<unsigned char>(mode);
        Check(resolve() == shared, "both native model formats resolve a cross-slot shared toon through the filename table");
        Check(color(.125f, .25f, .5f), "cross-slot shader color uses the same shared table entry as its texture");
        std::strcpy(model->pmdToonFileNames[0], "custom.bmp");
        Check(resolve() == custom, "a native PMD or PMX shared-table index changed to custom resolves the model-relative cache");
        Check(color(17.f/256, 34.f/256, 51.f/256), "PMD and PMX custom table overrides use the same cached shader color");
        std::strcpy(model->pmdToonFileNames[0], "toon02.bmp");
    }
    model->physicsMode = 2; material.toonReference = 0xfe;
    std::wcscpy(material.toonPath, L"D:\\model\\pmx-own.bmp");
    Check(resolve() == own, "independent negative PMX toon index retains its own material path");
    Check(color(170.f/256, 187.f/256, 204.f/256), "independent PMX toon tint retains its own material cache");
    material.toonReference = 0; std::strcpy(model->pmdToonFileNames[0], "custom.bmp");
    Check(resolve() == custom, "a PMX shared-table override never substitutes its unrelated own material path");
    const std::wstring unicodeName = L"日陰テスト.bmp";
    std::string cp932; Check(text_encoding::EncodeExact(unicodeName.c_str(), 932, cp932), "encode a real CP932 toon filename");
    const std::wstring path = std::wstring(model->modelDirectory) + unicodeName;
    auto* japanese = new ImportTestTexture(fixture.state); add(path.c_str(), japanese);
    fixture.renderer->resourcePool[4].tag = 0x00665544;
    std::strcpy(model->pmdToonFileNames[0], cp932.c_str());
    Check(resolve() == japanese, "native toon filenames resolve through exact CP932 independently of the Windows ACP");
    Check(color(68.f/256, 85.f/256, 102.f/256), "shader color and texture agree on a CP932 custom filename");
    const std::wstring longDirectory(240, L'd');
    const std::string longName(99, 't'); const std::wstring longPath = longDirectory + std::wstring(99, L't');
    auto* longTexture = new ImportTestTexture(fixture.state); add(longPath.c_str(), longTexture);
    fixture.renderer->resourcePool[5].tag = 0x00998877;
    std::wcscpy(model->modelDirectory, longDirectory.c_str()); std::strcpy(model->pmdToonFileNames[0], longName.c_str());
    Check(resolve() == longTexture, "full directory plus a 99-byte toon filename exceeds 256 without truncation");
    Check(color(119.f/256, 136.f/256, 153.f/256), "shader color uses the entire long model-relative toon path");
    failAfter = 0; const auto* withoutAllocation = resolve(); const bool colorWithoutAllocation = color(119.f/256, 136.f/256, 153.f/256); failAfter = -1;
    Check(withoutAllocation == longTexture && colorWithoutAllocation, "cached native toon texture and color resolution allocate no C++ strings each frame");
    std::memset(model->pmdToonFileNames[0], 0, 100); model->pmdToonFileNames[0][0] = char(0x82);
    Check(resolve() == nullptr, "a truncated CP932 toon path never selects an unrelated cache entry");
    Check(color(1, 1, 1), "an invalid custom path leaves the shader tint at the neutral fallback");
    model->pmdToonFileNames[0][0] = 0;
    Check(resolve() == nullptr, "an empty custom toon is not interpreted as the model directory");
    std::wmemset(model->modelDirectory, L'd', 256); std::strcpy(model->pmdToonFileNames[0], "custom.bmp");
    Check(resolve() == nullptr, "unterminated source directory refuses before reading outside its native field");
    Check(custom->ReferenceCount() == 1 && own->ReferenceCount() == 1 && shared->ReferenceCount() == 1,
        "renderer toon resolution borrows its selected texture without altering ownership");
    for (unsigned i = 0; i < 11; ++i) app->ToonTexture(i) = nullptr;
}
}
