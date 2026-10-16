// Include inside the test namespace after Fixture and Check in
// bone_key_pool_tests.cpp, then call BoneKeyPoolIdentityChecks() from main.
// Pool identity is process-local and typed; no identity values are persisted.
void BoneKeyPoolIdentityChecks() {
    using namespace mikudancestudio;
    Check(mdl::BoneKeyPoolIdentity(nullptr) == 0,
          "null bone pool has no strong identity");
    {
        std::array<mdl::BoneKey, 8> borrowed{};
        borrowed[3].frame = 71;
        auto* alias = borrowed.data();
        Check(mdl::BoneKeyPoolIdentity(alias) == 0,
              "raw bone array does not acquire a strong identity by lookup");
        mdl::BoneKeyPoolDestroy(alias);
        Check(alias == nullptr && borrowed[3].frame == 71 &&
                  mdl::BoneKeyPoolIdentity(borrowed.data()) == 0,
              "destroying a borrowed handle neither registers nor frees its storage");
    }
    {
        Fixture fixture(64);
        const auto identity = mdl::BoneKeyPoolIdentity(fixture.model->boneKeys);
        Check(identity != 0, "created bone pool has a strong identity");
        const auto guard = [&](const mdl::BoneKey* pool) {
            return mdl::BoneKeyPoolIdentity(pool) == identity &&
                mdl::BoneKeyCapacity(pool) >= 64;
        };
        auto* original = fixture.model->boneKeys;
        fixture.model->boneKeys[7].frame = 41;
        fixture.model->keyVisitMap[7] = 1;
        Check(mdl::BoneKeyPoolEnsure(*fixture.model, 64) &&
                  fixture.model->boneKeys == original && guard(original),
              "no-op ensure preserves a bone pool identity");
        Check(mdl::BoneKeyPoolEnsure(*fixture.model, 65) &&
                  fixture.model->boneKeys != original && guard(fixture.model->boneKeys) &&
                  mdl::BoneKeyPoolIdentity(original) == 0,
              "growth transfers the identity and retires the old allocation entry");
        Check(fixture.model->boneKeys[7].frame == 41 &&
                  mdl::BoneKeyVisitMapOf(*fixture.model).bytes[7] == 1,
              "identity transfer preserves native keys and visit marks");
        auto* grown = fixture.model->boneKeys;
        const auto capacity = fixture.Capacity();
        Check(mdl::BoneKeyPoolEnsure(*fixture.model, std::uint64_t(capacity) + 1) &&
                  fixture.model->boneKeys != grown && guard(fixture.model->boneKeys) &&
                  mdl::BoneKeyPoolIdentity(grown) == 0,
              "a second growth still represents the same logical bone pool");
        grown = fixture.model->boneKeys;
        Check(!mdl::BoneKeyPoolEnsure(*fixture.model,
                  std::uint64_t(mdl::kBoneKeyCapacityLimit) + 1) &&
                  fixture.model->boneKeys == grown && guard(grown),
              "refused growth preserves the allocation and its identity");
        mdl::BoneKeyPoolDestroy(fixture.model->boneKeys);
        Check(mdl::BoneKeyPoolIdentity(grown) == 0 && !guard(grown),
              "destroy invalidates an identity guard without dereferencing freed storage");
        fixture.model->boneKeys = mdl::BoneKeyPoolCreate(64);
        Check(mdl::BoneKeyPoolIdentity(fixture.model->boneKeys) != 0 &&
                  !guard(fixture.model->boneKeys),
              "a replacement pool never inherits the destroyed pool identity");
    }
    {
        // Every new ownership gets a fresh generation, including any address
        // the allocator recycles. Do not require a particular CRT reuse policy.
        std::array<const mdl::BoneKey*, 128> addresses{};
        std::array<std::uint64_t, 128> identities{};
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            auto* pool = mdl::BoneKeyPoolCreate(8);
            const auto identity = mdl::BoneKeyPoolIdentity(pool);
            bool fresh = identity != 0, recycledAddressFresh = true;
            for (std::size_t j = 0; j < i; ++j) {
                fresh = fresh && identity != identities[j];
                if (pool == addresses[j])
                    recycledAddressFresh = recycledAddressFresh && identity != identities[j];
            }
            Check(fresh && recycledAddressFresh,
                  "repeated create/destroy never revives an old identity at a reused address");
            addresses[i] = pool; identities[i] = identity;
            mdl::BoneKeyPoolDestroy(pool);
            Check(mdl::BoneKeyPoolIdentity(addresses[i]) == 0,
                  "each destroyed bone allocation is removed from identity lookup");
        }
    }
    {
        // Borrowed growth must establish new ownership without changing the
        // owner's array or claiming that its weak identity survived the move.
        auto model = std::make_unique<mdl::ModelRecord>();
        std::vector<mdl::BoneKey> borrowed(mdl::kBoneKeyCapacity);
        borrowed[7].frame = 93;
        model->boneKeys = borrowed.data();
        Check(mdl::BoneKeyPoolIdentity(model->boneKeys) == 0 &&
                  mdl::BoneKeyPoolEnsure(*model, mdl::kBoneKeyCapacity) &&
                  mdl::BoneKeyPoolIdentity(model->boneKeys) == 0,
              "a no-op ensure leaves borrowed identity weak");
        const bool adopted = mdl::BoneKeyPoolEnsure(*model,
            std::uint64_t(mdl::kBoneKeyCapacity) + 1);
        const auto identity = mdl::BoneKeyPoolIdentity(model->boneKeys);
        Check(adopted && model->boneKeys != borrowed.data() && identity != 0 &&
                  mdl::BoneKeyPoolIdentity(borrowed.data()) == 0 &&
                  borrowed[7].frame == 93 && model->boneKeys[7].frame == 93,
              "borrowed growth creates a distinct owned identity and preserves the source");
        mdl::BoneKeyPoolDestroy(model->boneKeys);
        Check(borrowed[7].frame == 93,
              "destroying the adopted pool leaves the borrowed source alive");
    }
}
