// Include at file scope after Check/failAfter in native_edit_history_tests.cpp;
// the parent registers NativeSceneIdentityAllocationChecks(). No scene data is needed.
#include "../src/plugins/scene_identity.hpp"

namespace {
void NativeSceneIdentityAllocationChecks() {
    using namespace mikumikustudio::plugins;
    struct AllocationLimit {
        long previous;
        explicit AllocationLimit(long limit) : previous(failAfter) { failAfter = limit; }
        ~AllocationLimit() { failAfter = previous; }
    };

    // A throwing Get never exposes its random ID. Count live allocations while
    // the registry is still alive to detect orphan nodes without private-field
    // access or a production-only test API.
    for (bool seeded : {false, true}) {
        bool completed = false, sawGenerationGap = false;
        long failures = 0;
        // Sweep to the first success instead of assuming STL node/sentinel or
        // debug-proxy counts. This includes emplace failures after known insert.
        for (long limit = 0; limit < 64; ++limit) {
            SceneIdentity identities;
            int existingToken = 0, requestedToken = 0, nextToken = 0;
            const SceneIdentity::Entry* existing = nullptr;
            MmsObjectRef before{};
            MmsId element{};
            if (seeded) {
                existing = &identities.Get(&existingToken);
                before = existing->object;
                element = identities.Element(&existingToken, MMS_CHANNEL_BONES, 0);
            }

            const SceneIdentity::Entry* registered = nullptr;
            bool memory = false;
            long remaining = -1;
            AllocationAudit allocations;
            {
                AllocationLimit injection(limit);
                allocationAudit=&allocations;
                try { registered = &identities.Get(&requestedToken); }
                catch (const std::bad_alloc&) { memory = true; }
                allocationAudit=nullptr;
                remaining = failAfter;
            }
            Check(!allocations.overflow,"bounded identity allocation audit fits its fixed storage");
            if (seeded) {
                Check(identities.Retained(&existingToken) == existing && Same(existing->object, before) &&
                    existing->present && existing->published && identities.Known(before.id),
                    "Get allocation sweep preserves existing entry address, reference and known ID");
                Check(existing->elements.size() == 1 && Same(existing->elements.begin()->second, element),
                    "Get allocation sweep preserves existing element identities");
                AllocationLimit injection(0);
                Check(&identities.Get(&existingToken) == existing &&
                    Same(identities.Element(&existingToken, MMS_CHANNEL_BONES, 0), element),
                    "existing identity and element lookups remain allocation-free after Get attempt");
            }
            if (memory) {
                ++failures;
                Check(allocations.outstanding==0,
                    "failed Get releases every new allocation while the registry remains alive");
                Check(remaining == 0 && !registered && !identities.Retained(&requestedToken),
                    "injected Get bad_alloc propagates without retaining a partial entry");
                registered = &identities.Get(&requestedToken);
                sawGenerationGap = sawGenerationGap || registered->object.generation > before.generation + 1;
            }
            Check(registered && identities.Retained(&requestedToken) == registered &&
                !Empty(registered->object.id) && identities.Known(registered->object.id) &&
                registered->object.generation > before.generation && registered->present &&
                registered->published && registered->elements.empty() && !Same(registered->object.id, before.id),
                "Get success or OOM retry registers a fresh, present, published identity");
            const auto current = registered->object;
            {
                AllocationLimit injection(0);
                Check(&identities.Get(&requestedToken) == registered && Same(registered->object, current),
                    "repeated Get after success or OOM retry reuses the entry without allocating");
            }
            const auto next = identities.Get(&nextToken).object;
            Check(next.generation > current.generation && !Same(next.id, current.id) &&
                !Same(next.id, before.id) && identities.Known(next.id) && identities.Known(current.id) &&
                (!seeded || identities.Known(before.id)),
                "subsequent registration advances generation and preserves distinct registered IDs");
            if (!memory) {
                Check(failures >= 2 && sawGenerationGap,
                    "Get sweep reaches node allocation failures and preserves reserved generation gaps");
                completed = true;
                break;
            }
        }
        Check(completed, "Get allocation sweep reaches success within its bounded fixture");
    }
}
}
