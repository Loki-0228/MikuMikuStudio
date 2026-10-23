// MAINTENANCE: regression-keep
// Included inside the renderer policy test's anonymous namespace, after its
// Host/Model helpers. Draws captures arguments BEFORE the native DIP call;
// Frame's real target readback supplies separate rasterization evidence.
#pragma once

void VisibilityUpload(Host& host, Model& model) {
    void* destination = nullptr;
    const UINT bytes = static_cast<UINT>(model.vertices.size() * sizeof(Vertex));
    Hr(model.vb->Lock(0, bytes, &destination, 0), "lock visibility fixture source VB");
    std::memcpy(destination, model.vertices.data(), bytes);
    Hr(model.vb->Unlock(), "upload visibility fixture source VB");
    (void)host;
}

void VisibilityTriangle(Model& model, unsigned face,
                        const std::array<std::array<float, 3>, 3>& points) {
    for (unsigned corner = 0; corner < 3; ++corner) {
        auto& vertex = model.vertices[model.faces[face][corner]];
        vertex.x = points[corner][0];
        vertex.y = points[corner][1];
        vertex.z = points[corner][2];
    }
}

void VisibilityWhitePixel(const Host& host, int resource, unsigned layers) {
    if (resource) return; // Effect lighting has its own real non-background check.
    // Half-alpha white over black, with ZWRITE enabled. A later farther face
    // fails depth, so only the explicitly counted nearward layers contribute.
    const int expected = layers == 1 ? 128 : 191;
    Check(layers == 1 || layers == 2, "visibility pixel oracle has an explicit layer count");
    for (unsigned shift : {0u, 8u, 16u})
        Check(std::abs(int((host.lastPixel >> shift) & 255) - expected) <= 2,
              "visibility fixed pixel matches independent depth and blend oracle");
}

void VisibilityRendererChecks(Host& host, Uploads& uploads, Draws& draws,
                              bool pmx, int resource) {
    struct Tier {
        TransparentSortPrecision precision;
        float nearDepth, farDepth;
        unsigned commonBin;
    };
    // Authored depth bounds are 0..1. The independently calculated forward
    // bins floor(z*N) are respectively 8, 32 and 128 for BOTH visible faces.
    // If outside anchors are discarded before bounds are calculated, those
    // two faces become the extremes and reverse. No production sorter/helper
    // supplies these expected orders.
    const Tier tiers[] = {
        {TransparentSortPrecision::Low, .51f, .53f, 8},
        {TransparentSortPrecision::Standard, .510f, .515f, 32},
        {TransparentSortPrecision::High, .5005f, .501f, 128}
    };
    for (const auto& tier : tiers) {
        auto model = MakeModel(host, pmx, pmx ? 65536u : 0u,
                               {tier.nearDepth, 0.f, tier.farDepth, 1.f});
        // Release device bindings before destroying the owned model, including
        // when a Check throws. No test-owned pointer survives in model slots.
        struct Unbind { Host& host; ~Unbind() { host.Unbind(); } } unbind{host};
        model->record->comboSelIndex = 0;
        host.app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model->record.get());
        host.app->ModelSlot(1) = nullptr;
        host.app->TransparentMaterialSortPrecision() = tier.precision;
        for (bool cross : {false, true}) {
            host.app->TransparentSortCrossObjects() = cross;
            // Outside -> visible away from the center -> outside exercises
            // collection/cache invalidation while leaving every depth intact.
            for (bool anchorsVisible : {false, true, false}) {
                for (unsigned face : {1u, 3u}) {
                    const float z = face == 1 ? 0.f : 1.f;
                    const float x = anchorsVisible ? .75f : 2.5f;
                    VisibilityTriangle(*model, face, {{{x, .75f, z},
                        {x, .95f, z}, {x + .2f, .75f, z}}});
                }
                VisibilityUpload(host, *model);
                uploads.Reset();
                std::printf("visibility-bounds pmx=%d resource=%d precision=%u cross=%d anchors-visible=%d manual-bin=%u\n",
                    pmx, resource, unsigned(tier.precision), cross, anchorsVisible, tier.commonBin);
                host.Frame(draws, uploads, resource);
                const std::vector<Expected> expected = anchorsVisible
                    ? std::vector<Expected>{{model.get(), 3}, {model.get(), 0},
                                            {model.get(), 2}, {model.get(), 1}}
                    : std::vector<Expected>{{model.get(), 0}, {model.get(), 2}};
                CheckFaces(draws, expected, true);
                VisibilityWhitePixel(host, resource, 1);
                SourceUnchanged(*model, pmx);
            }
        }
    }

    // Face 0 is the central near face. Faces 1..6 each share one outside
    // clip plane. Face 7's vertices are individually outside different planes,
    // but its interior covers the center: rejecting it by vertex visibility
    // instead of a shared-plane test is incorrect.
    auto model = MakeModel(host, pmx, pmx ? 65536u : 0u,
                           {.4f, .6f, .6f, .6f, .6f, -.25f, 1.25f, .6f});
    struct UnbindPlanes { Host& host; ~UnbindPlanes() { host.Unbind(); } } unbind{host};
    model->record->comboSelIndex = 0;
    host.app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model->record.get());
    host.app->ModelSlot(1) = nullptr;
    for (unsigned face : {1u, 2u, 3u, 4u}) {
        for (UINT index : model->faces[face]) {
            auto& vertex = model->vertices[index];
            if (face == 1) vertex.x -= 3.f; // all x < -w
            if (face == 2) vertex.x += 3.f; // all x > w
            if (face == 3) vertex.y -= 3.f; // all y < -w
            if (face == 4) vertex.y += 3.f; // all y > w
        }
    }
    // Face 5 has all z < 0; face 6 has all z > w, with identity projection.
    VisibilityTriangle(*model, 7, {{{-2.f, -2.f, .6f},
                                  {0.f, 2.f, .6f}, {2.f, -2.f, .6f}}});
    VisibilityUpload(host, *model);
    for (auto precision : {TransparentSortPrecision::Low, TransparentSortPrecision::Standard,
                            TransparentSortPrecision::High, TransparentSortPrecision::Off}) {
        host.app->TransparentMaterialSortPrecision() = precision;
        for (bool cross : {false, true}) {
            host.app->TransparentSortCrossObjects() = cross;
            uploads.Reset();
            std::printf("visibility-six-planes pmx=%d resource=%d precision=%u cross=%d\n",
                        pmx, resource, unsigned(precision), cross);
            host.Frame(draws, uploads, resource);
            std::vector<Expected> expected;
            if (precision == TransparentSortPrecision::Off) {
                for (unsigned face = 0; face < 8; ++face) expected.emplace_back(model.get(), face);
            } else {
                // Bounds [-.25,1.25] keep .6 and .4 in distinct bins even in
                // Low: floor((.6+.25)/1.5*16)=9; floor((.4+.25)/1.5*16)=6.
                expected = {{model.get(), 7}, {model.get(), 0}};
            }
            CheckFaces(draws, expected, precision != TransparentSortPrecision::Off);
            VisibilityWhitePixel(host, resource, precision == TransparentSortPrecision::Off ? 1 : 2);
            SourceUnchanged(*model, pmx);
        }
    }
}
