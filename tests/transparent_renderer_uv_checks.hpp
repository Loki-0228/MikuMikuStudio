#pragma once
// Private include for transparent_renderer_policy_tests.cpp. Include after
// Host/Model/Uploads/Draws/MakeModel/Install, in their anonymous namespace.
// Not an SDK header; it changes no shared capture class and adds no production
// seam. Every helper below carries the Uv prefix so it cannot collide with the
// existing fixture (MakeModel/CheckFaces/SourceUnchanged stay untouched).
//
// Scope: with the real device, two real PMX models (two materials each,
// nonsequential source indices above 65535) and 1..4 PMX additional-UV streams
// per vertex, the lower three transparent tiers submit exactly one private
// ordering index buffer and the Off tier keeps the authored index buffer, while
// the source vertex buffer keeps its replaced stride, every authored UV stream
// and every authored index untouched. A second DrawObserver reads the device
// FVF at the actual DIP, so a wrong SetFVF cannot hide behind a correct stride;
// it owns the second observation slot because no Maximum observer exists in
// this mode. The extra UV bytes are never claimed to be sampled by the built-in
// fixed or effect shaders: no visual semantics are certified here.

struct UvVertexFormat {
    DWORD fvf;
    UINT stride;
};

UvVertexFormat UvFormat(unsigned additionalUv) {
    // Base layout: position, normal and one texture coordinate (32 bytes) as
    // created by MakeModel; every additional PMX UV is a separate 4-float
    // texture coordinate set, exactly like ModelVertexFormat() composes it.
    DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL |
        (DWORD(1 + additionalUv) << D3DFVF_TEXCOUNT_SHIFT);
    for (unsigned index = 1; index <= additionalUv; ++index)
        fvf |= D3DFVF_TEXCOORDSIZE4(index);
    return {fvf, static_cast<UINT>(sizeof(Vertex)) + 16u * additionalUv};
}

void UvCheckFormatParity() {
    // ModelVertexFormat() in src/render/model_renderers.cpp hard-codes these
    // FVF/stride pairs for pmxAdditionalUvCount 1..4. Composing the same FVF
    // from the D3D9 macros must reproduce them, so this fixture cannot drift
    // from the production layout it exercises.
    struct UvProductionFormat { unsigned additionalUv; DWORD fvf; UINT stride; };
    const UvProductionFormat production[] = {
        {1, 524818, 48}, {2, 2622226, 64},
        {3, 11011090, 80}, {4, 44565778, 96}};
    for (const auto& row : production) {
        const UvVertexFormat format = UvFormat(row.additionalUv);
        Check(format.fvf == row.fvf && format.stride == row.stride,
              "extra-UV fixture layout matches production ModelVertexFormat");
    }
}

float UvSentinel(std::size_t vertex, unsigned uv, unsigned component) {
    // Distinct, finite values keep every extra component diagnostic: a wrong
    // stride or a shifted UV set cannot reproduce the expected byte array.
    const double value = 1.0 + static_cast<double>(vertex) * 0.001 +
        static_cast<double>(uv) * 1000.0 + static_cast<double>(component) * 100.0;
    return static_cast<float>(value);
}

struct UvCreatedVertexBuffer {
    IDirect3DVertexBuffer9* buffer = nullptr;
    ~UvCreatedVertexBuffer() { if (buffer) buffer->Release(); }
    IDirect3DVertexBuffer9* Detach() { auto* result = buffer; buffer = nullptr; return result; }
};

// Owns one source vertex buffer with 1..4 additional PMX UV streams. The model
// itself keeps releasing vertexBuffer/indexBuffer in its own destructor, so the
// buffer it was born with is released exactly once here, before the extra
// buffer becomes model->vb; model->record->vertexBuffer is rebound to it.
struct UvExtraModel {
    std::unique_ptr<Model> model;
    std::vector<unsigned char> vertexBytes;
    std::vector<INT> authorIndices;
    unsigned additionalUv = 0;
    UINT stride = 0;
    DWORD fvf = 0;

    UvExtraModel(Host& host, std::unique_ptr<Model> source, unsigned uv)
        : model(std::move(source)), additionalUv(uv) {
        Check(model && model->record, "extra-UV fixture needs a prepared model");
        Check(model->record->physicsMode == 2, "extra-UV fixture uses the real PMX state");
        Check(model->record->pmxAdditionalUvCount == 0,
              "extra-UV fixture replaces a plain PMX source");
        Check(additionalUv >= 1 && additionalUv <= 4,
              "extra-UV fixture covers the PMX 1..4 range");
        Check(model->record->vertexCount == model->vertices.size(),
              "extra-UV fixture knows the authored vertex count");
        const UvVertexFormat format = UvFormat(additionalUv);
        fvf = format.fvf;
        stride = format.stride;
        authorIndices = model->pmx;
        vertexBytes.assign(static_cast<std::size_t>(model->vertices.size()) * stride, 0);
        for (std::size_t vertex = 0; vertex < model->vertices.size(); ++vertex) {
            unsigned char* destination = vertexBytes.data() + vertex * stride;
            std::memcpy(destination, &model->vertices[vertex], sizeof(Vertex));
            for (unsigned set = 0; set < additionalUv; ++set)
                for (unsigned component = 0; component < 4; ++component) {
                    const float sentinel = UvSentinel(vertex, set, component);
                    std::memcpy(destination + sizeof(Vertex) + set * 16u + component * 4u,
                                &sentinel, sizeof(sentinel));
                }
        }
        UvCreatedVertexBuffer created;
        Hr(host.device->CreateVertexBuffer(static_cast<UINT>(vertexBytes.size()), 0, fvf,
                                           D3DPOOL_MANAGED, &created.buffer, nullptr),
           "create extra-UV source vertex buffer");
        D3DVERTEXBUFFER_DESC description{};
        Hr(created.buffer->GetDesc(&description), "query extra-UV source vertex buffer");
        Check(description.Size == static_cast<UINT>(vertexBytes.size()),
              "extra-UV source buffer holds exactly the authored byte array");
        void* upload = nullptr;
        Hr(created.buffer->Lock(0, 0, &upload, 0), "lock extra-UV source vertex buffer");
        std::memcpy(upload, vertexBytes.data(), vertexBytes.size());
        const HRESULT unlocked = created.buffer->Unlock();
        Hr(unlocked, "unlock extra-UV source vertex buffer");
        IDirect3DVertexBuffer9* replacement = created.Detach();
        Release(model->vb);
        model->vb = replacement;
        model->record->vertexBuffer = replacement;
        model->record->pmxAdditionalUvCount = static_cast<std::uint8_t>(additionalUv);
        Check(model->record->vertexBuffer == model->vb &&
                  model->record->pmxAdditionalUvCount == additionalUv,
              "extra-UV fixture publishes the replaced source vertex buffer");
    }
    ~UvExtraModel() = default;  // Model releases the replaced VB and source IB once.
};

struct UvFaceExpectation {
    Model* model;
    unsigned material;
};

const UvExtraModel* UvFind(const std::vector<const UvExtraModel*>& sources, Model* model) {
    for (const UvExtraModel* source : sources)
        if (source->model.get() == model) return source;
    return nullptr;
}

void UvCheckFaces(const Draws& draws, const std::vector<UvFaceExpectation>& expected,
                  const std::vector<const UvExtraModel*>& sources, bool sorted) {
    Check(!draws.error, draws.error ? draws.error : "extra-UV draw observer healthy");
    Check(draws.count == expected.size(),
          "extra-UV frame submits exactly one actual triangle per expected face");
    Check(!sources.empty(), "extra-UV fixture has authored objects to compare against");
    IDirect3DIndexBuffer9* privateBuffer = nullptr;
    for (unsigned index = 0; index < draws.count; ++index) {
        const auto& actual = draws.faces[index];
        const auto& wanted = expected[index];
        const UvExtraModel* source = UvFind(sources, wanted.model);
        Check(source != nullptr, "extra-UV draw belongs to a known authored model");
        Check(actual.object == wanted.model->record.get(),
              "extra-UV draw preserves authored object scope/order");
        Check(actual.material == wanted.material,
              "extra-UV draw preserves authored material identity/order");
        Check(actual.vb == source->model->vb,
              "extra-UV draw binds the replaced source vertex buffer");
        Check(actual.stride == source->stride &&
                  actual.stride == UvFormat(source->additionalUv).stride,
              "extra-UV draw uses the production stride for its PMX UV count");
        Check(actual.indices == wanted.model->faces[wanted.material],
              "extra-UV draw keeps nonsequential authored source indices");
        if (sorted) {
            Check(actual.scratch,
                  "sorted extra-UV tier allocates a private ordering index buffer");
            Check(actual.ib != wanted.model->ib,
                  "sorted extra-UV tier does not reuse the authored source IB");
            Check(actual.ib != nullptr, "sorted extra-UV draw binds an index buffer");
            if (!privateBuffer) privateBuffer = actual.ib;
            Check(actual.ib == privateBuffer,
                  "one private ordering IB serves every sorted extra-UV face");
        } else {
            Check(!actual.scratch, "Off uses no private ordering index buffer");
            Check(actual.ib == wanted.model->ib,
                  "Off keeps the authored source index buffer");
        }
    }
    if (sorted) {
        Check(privateBuffer != nullptr,
              "sorted extra-UV frame produced a private ordering index buffer");
        for (const UvExtraModel* source : sources)
            Check(privateBuffer != source->model->ib,
                  "private ordering IB is distinct from every authored source IB");
    }
}

void UvVerifySource(const UvExtraModel& source) {
    const Model& model = *source.model;
    Check(model.record->vertexBuffer == model.vb,
          "extra-UV model keeps the replaced source vertex buffer bound");
    Check(model.record->indexBuffer == model.ib,
          "extra-UV model keeps its authored source index buffer bound");
    Check(model.record->pmxAdditionalUvCount == source.additionalUv,
          "extra-UV model keeps its PMX additional-UV count");
    Check(model.record->vertexCount == model.vertices.size(),
          "extra-UV model keeps its authored vertex count");
    void* memory = nullptr;
    Hr(model.vb->Lock(0, 0, &memory, D3DLOCK_READONLY),
       "read extra-UV source vertex buffer after frame");
    const bool sameBytes =
        std::memcmp(memory, source.vertexBytes.data(), source.vertexBytes.size()) == 0;
    const HRESULT unlocked = model.vb->Unlock();
    Hr(unlocked, "unlock extra-UV source vertex buffer after frame");
    Check(sameBytes,
          "extra-UV source bytes, including every sentinel UV component, are unchanged");
    Check(model.pmx.size() == source.authorIndices.size() &&
              std::memcmp(model.pmx.data(), source.authorIndices.data(),
                          source.authorIndices.size() * sizeof(INT)) == 0,
          "extra-UV scene keeps the authored PMX index array unchanged");
    Hr(model.ib->Lock(0, 0, &memory, D3DLOCK_READONLY),
       "read authored source index buffer after frame");
    bool sameIndices = true;
    std::size_t position = 0;
    for (const auto& face : model.faces) for (UINT wanted : face) {
        INT actual = 0;
        std::memcpy(&actual, static_cast<const unsigned char*>(memory) +
                                sizeof(INT) * position, sizeof(INT));
        ++position;
        sameIndices = sameIndices && actual == static_cast<INT>(wanted);
    }
    const HRESULT indexUnlocked = model.ib->Unlock();
    Hr(indexUnlocked, "unlock authored source index buffer after frame");
    Check(sameIndices,
          "extra-UV scene keeps the real source index buffer bytes unchanged");
}

// Second DrawObserver for the same pre-DIP point. The base observer proves the
// object/material/VB/IB/stride identity, but a wrong SetFVF would still pass
// that evidence; this one queries the device FVF and compares it with the FVF
// the fixture created for the object. Callbacks are noexcept and only record an
// error; the parent checks it after the real frame.
class UvFvfObserver : public deformed_mesh_capture::DrawObserver {
public:
    UvFvfObserver(MMDApp& app, IDirect3DDevice9* owned,
                  const std::vector<const UvExtraModel*>& sourceList)
        : device(owned), sources(sourceList) {
        std::string reason;
        Check(deformed_mesh_capture::Observe(app, this, reason),
              reason.empty() ? "install extra-UV FVF observer" : reason.c_str());
        observing = true;
    }
    ~UvFvfObserver() { if (observing) deformed_mesh_capture::StopObserving(this); }
    UvFvfObserver(const UvFvfObserver&) = delete;
    UvFvfObserver& operator=(const UvFvfObserver&) = delete;
    void Reset() { error = nullptr; observed = 0; }
    void Verify(unsigned expectedDraws) const {
        Check(!error, error ? error : "extra-UV FVF observer healthy");
        Check(observed == expectedDraws,
              "every actual extra-UV draw reports the fixture FVF");
    }
    void Indexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT minimum,
                 UINT vertexCount, UINT start, UINT primitives, const void* object,
                 unsigned material) noexcept override {
        if (d != device || !object || error) return;
        if (type != D3DPT_TRIANGLELIST) {
            error = "unexpected extra-UV FVF topology";
            return;
        }
        DWORD fvf = 0;
        const HRESULT result = d->GetFVF(&fvf);
        if (FAILED(result)) {
            error = "actual extra-UV draw could not query device FVF";
            return;
        }
        const UvExtraModel* source = nullptr;
        for (const UvExtraModel* candidate : sources)
            if (object == static_cast<const void*>(candidate->model->record.get()))
                source = candidate;
        if (!source) {
            error = "actual extra-UV draw has no fixture FVF";
            return;
        }
        if (fvf != source->fvf) {
            error = "actual extra-UV draw FVF omits an extra-UV texture coordinate";
            return;
        }
        ++observed;
    }
    void Unsupported(IDirect3DDevice9* d, D3DPRIMITIVETYPE, UINT) noexcept override {
        if (d == device) error = "unexpected unindexed submission during extra-UV FVF check";
    }
private:
    IDirect3DDevice9* device;
    std::vector<const UvExtraModel*> sources;
    const char* error = nullptr;
    unsigned observed = 0;
    bool observing = false;
};

void UvRunFrame(Host& host, Uploads& uploads, Draws& draws, int resource,
                unsigned additionalUv, TransparentSortPrecision precision, bool cross) {
    host.app->TransparentMaterialSortPrecision() = precision;
    host.app->TransparentSortCrossObjects() = cross;
    uploads.Reset();
    std::printf("uv pmx=1 resource=%d extra-uv=%u precision=%u cross=%d\n",
                resource, additionalUv, unsigned(precision), cross ? 1 : 0);
    host.Frame(draws, uploads, resource);
}

// Parent integration: call once per requested renderer after host.Effect() has
// loaded it, with the base Uploads and Draws already active. The caller keeps
// owning the device/slots; this helper only swaps each model's source VB for an
// extra-UV one and clears the slots before releasing the models.
void UvRendererChecks(Host& host, Uploads& uploads, Draws& draws, int resource) {
    Check(resource == 0 || resource == 117 || resource == 118,
          "extra-UV fixture accepts only the known native renderers");
    Check(resource == 0 || host.renderer->effect != nullptr,
          "extra-UV effect checks need the requested effect already loaded");
    UvCheckFormatParity();
    for (unsigned additionalUv = 1; additionalUv <= 4; ++additionalUv) {
        UvExtraModel a(host, MakeModel(host, true, 65536, {.2f, .8f}), additionalUv);
        UvExtraModel b(host, MakeModel(host, true, 65552, {.4f, .6f}), additionalUv);
        Install(host, *a.model, *b.model);
        const std::vector<const UvExtraModel*> sources = {&a, &b};
        UvFvfObserver fvfObserver(*host.app, host.device, sources);
        const std::vector<UvFaceExpectation> authored = {
            {a.model.get(), 0}, {a.model.get(), 1},
            {b.model.get(), 0}, {b.model.get(), 1}};
        const std::vector<UvFaceExpectation> local = {
            {a.model.get(), 1}, {a.model.get(), 0},
            {b.model.get(), 1}, {b.model.get(), 0}};
        const std::vector<UvFaceExpectation> global = {
            {a.model.get(), 1}, {b.model.get(), 1},
            {b.model.get(), 0}, {a.model.get(), 0}};
        for (auto precision : {TransparentSortPrecision::Low,
                               TransparentSortPrecision::Standard,
                               TransparentSortPrecision::High}) {
            for (bool cross : {false, true}) {
                fvfObserver.Reset();
                UvRunFrame(host, uploads, draws, resource, additionalUv, precision, cross);
                UvCheckFaces(draws, cross ? global : local, sources, true);
                fvfObserver.Verify(uploads.draws);
                UvVerifySource(a);
                UvVerifySource(b);
            }
        }
        fvfObserver.Reset();
        UvRunFrame(host, uploads, draws, resource, additionalUv,
                   TransparentSortPrecision::Off, false);
        UvCheckFaces(draws, authored, sources, false);
        fvfObserver.Verify(uploads.draws);
        UvVerifySource(a);
        UvVerifySource(b);
        host.Unbind();
    }
    host.Unbind();
}
