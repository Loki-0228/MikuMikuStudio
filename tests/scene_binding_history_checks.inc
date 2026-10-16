// Logical binding locations survive registered pool growth. These fixtures
// also exercise replacement and identity changes without depending on malloc
// returning a particular address.
void RemovalBindingsGrowth() {
    using namespace plugins;
    Fixture deleted, peer;
    auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
    app->ModelSlot(3) = deleted.raw(); app->ModelSlot(7) = peer.raw();
    peer.m->comboSelIndex = peer.m->comboSelIndex2 = 1;
    mdl::BoneOrderEntry order{}; order.linkedModel = 3; order.linkedBone = 11;
    peer.m->boneOrderTable = &order;
    for (unsigned i = 0; i < 4; ++i) peer.m->displayKeys[i].selectorStates[0] = {-1, 0};
    peer.m->displayKeys[2].selectorStates[0] = {3, 17};
    app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4);
    for (unsigned i = 0; i < 4; ++i) app->CameraKeys()[i].parentModel = -1;
    app->CameraKeys()[1].parentModel = 3; app->CameraKeys()[1].parentBone = 23;
    app->CameraParentModel() = -1;
    mdl::AccessoryRecord accessory{}, later{}; accessory.parentModel = 3; accessory.parentBone = 29;
    accessory.order = 0; later.order = 1; later.parentModel = -1;
    app->AccessorySlot(11) = &accessory; app->AccessorySlot(12) = &later;
    app->AccessoryKeys(11) = mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
    app->AccessoryKeys(12) = mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
    for (unsigned i = 0; i < 4; ++i) app->AccessoryKeys(11)[i].parentModel = app->AccessoryKeys(12)[i].parentModel = -1;
    auto& key = app->AccessoryKeys(11)[2]; key.parentModel = 3; key.parentBone = 31; key.visible = 1; key.scale = 4.5f;
    const auto displayIdentity = mdl::KeyPoolIdentity(peer.m->displayKeys);
    const auto cameraIdentity = mdl::KeyPoolIdentity(app->CameraKeys());
    const auto accessoryIdentity = mdl::KeyPoolIdentity(app->AccessoryKeys(11));
    Check(displayIdentity && cameraIdentity && accessoryIdentity, "registered generic pools receive nonzero identities");
    const auto grow = [&] {
        Check(mdl::DisplayKeyPoolEnsure(*peer.m, mdl::KeyPoolCapacity(peer.m->displayKeys) + 1), "grow real display pool with nested selectors");
        Check(mdl::KeyPoolEnsure(app->CameraKeys(), mdl::KeyPoolCapacity(app->CameraKeys()) + 1), "grow real camera pool");
        Check(mdl::KeyPoolEnsure(app->AccessoryKeys(11), mdl::KeyPoolCapacity(app->AccessoryKeys(11)) + 1), "grow real accessory pool");
        Check(mdl::KeyPoolIdentity(peer.m->displayKeys) == displayIdentity &&
            mdl::KeyPoolIdentity(app->CameraKeys()) == cameraIdentity &&
            mdl::KeyPoolIdentity(app->AccessoryKeys(11)) == accessoryIdentity, "all registered pool identities survive relocation");
    };
    auto change = CaptureModelRemovalBindings(*app, 3);
    StorageStore storage; SharedHistory history(storage); std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(history.PrepareNative("grow before commit", change, tx) == MMS_OK, "prepare binding addresses before growth");
    grow();
    Check(history.CommitNative(std::move(tx)) == MMS_E_STALE && history.UndoCount() == 0 &&
        app->CameraKeys()[1].parentModel == 3 && app->AccessoryKeys(11)[2].parentModel == 3,
        "moving prepared addresses rejects commit without partial writes");
    Check(history.PrepareNative("remove after growth", change, tx) == MMS_OK, "resolve the same logical binding record after growth");
    failAfter = 0; const auto commit = history.CommitNative(std::move(tx)); failAfter = -1;
    Check(commit == MMS_OK, "relocated binding publication does not allocate");
    const auto newDisplay = mdl::KeyPoolCapacity(peer.m->displayKeys);
    const auto newCamera = mdl::KeyPoolCapacity(app->CameraKeys());
    const auto newAccessory = mdl::KeyPoolCapacity(app->AccessoryKeys(11));
    grow();
    peer.m->displayKeys[newDisplay].selectorStates[0] = {91, 47};
    app->CameraKeys()[newCamera].parentModel = 92; app->CameraKeys()[newCamera].parentBone = 53;
    app->AccessoryKeys(11)[newAccessory].parentModel = 93; app->AccessoryKeys(11)[newAccessory].scale = 7.5f;
    app->AccessoryKeys(11)[2].scale = 9.75f;
    const auto unrelated = [&] {
        return peer.m->displayKeys[newDisplay].selectorStates[0].modelIndex == 91 &&
            peer.m->displayKeys[newDisplay].selectorStates[0].boneIndex == 47 &&
            app->CameraKeys()[newCamera].parentModel == 92 && app->CameraKeys()[newCamera].parentBone == 53 &&
            app->AccessoryKeys(11)[newAccessory].parentModel == 93 && app->AccessoryKeys(11)[newAccessory].scale == 7.5f &&
            app->AccessoryKeys(11)[2].scale == 9.75f;
    };
    unsigned failures = 0;
    for (long limit = 0; limit < 4096; ++limit) {
        failAfter = limit; const auto result = history.History(false); failAfter = -1;
        if (result == MMS_OK) break;
        ++failures;
        Check(result == MMS_E_MEMORY && history.UndoCount() == 1 && app->CameraKeys()[1].parentModel == -1 &&
            app->AccessoryKeys(11)[2].parentModel == -1 && unrelated(), "failed relocation preparation leaves scene and history unchanged");
        Check(limit < 4095, "relocated history failure sweep terminates");
    }
    Check(failures >= 2 && history.RedoCount() == 1 && order.linkedModel == 3 &&
        peer.m->displayKeys[2].selectorStates[0].modelIndex == 3 && app->CameraKeys()[1].parentModel == 3 &&
        app->AccessoryKeys(11)[2].parentModel == 3 && app->AccessoryKeys(11)[2].visible && unrelated(),
        "undo resolves grown pools and preserves new slots and unrelated values");
    Check(history.History(true) == MMS_OK && unrelated(), "redo through grown pools preserves unrelated values");

    auto* originalCamera = app->CameraKeys();
    auto* replacementCamera = mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(originalCamera));
    std::memcpy(replacementCamera, originalCamera, mdl::KeyPoolCapacity(originalCamera) * sizeof(mdl::CameraKey));
    Check(mdl::KeyPoolIdentity(replacementCamera) != cameraIdentity, "independent identical pool has a different identity");
    app->CameraKeys() = replacementCamera;
    Check(history.History(false) == MMS_E_STALE && history.UndoCount() == 1, "identical replacement camera pool cannot revive old history");
    app->CameraKeys() = originalCamera;
    // Deterministically model allocator ABA: the address and bytes stay equal,
    // but its registry identity is that of an independently created pool.
    {
        std::lock_guard<std::mutex> lock(mdl::KeyPoolStorage<mdl::CameraKey>::mutex);
        mdl::KeyPoolStorage<mdl::CameraKey>::capacities.at(originalCamera).identity =
            mdl::KeyPoolStorage<mdl::CameraKey>::capacities.at(replacementCamera).identity;
    }
    Check(history.History(false) == MMS_E_STALE && history.UndoCount() == 1 && unrelated(), "same-address pool identity ABA is rejected");
    {
        std::lock_guard<std::mutex> lock(mdl::KeyPoolStorage<mdl::CameraKey>::mutex);
        mdl::KeyPoolStorage<mdl::CameraKey>::capacities.at(originalCamera).identity = cameraIdentity;
    }
    mdl::KeyPoolDestroy(replacementCamera);
    const auto restoredCapacity = mdl::KeyPoolCapacity(originalCamera);
    auto* recreated = mdl::KeyPoolCreate<mdl::CameraKey>(restoredCapacity);
    const auto recreatedIdentity = mdl::KeyPoolIdentity(recreated); mdl::KeyPoolDestroy(recreated);
    recreated = mdl::KeyPoolCreate<mdl::CameraKey>(restoredCapacity);
    Check(mdl::KeyPoolIdentity(recreated) != recreatedIdentity, "destroy and create never recycle a generic pool identity");
    mdl::KeyPoolDestroy(recreated);
    auto* selector = peer.m->displayKeys[2].selectorStates;
    auto* selectorCopy = new mdl::BoneReference[1]{selector[0]}; peer.m->displayKeys[2].selectorStates = selectorCopy;
    Check(history.History(false) == MMS_E_STALE, "grown display pool still rejects replaced nested selector");
    peer.m->displayKeys[2].selectorStates = selector; delete[] selectorCopy;
    Check(history.History(false) == MMS_OK && unrelated(), "restoring valid owners permits undo again");

    auto accessoryOrder = CaptureAccessoryRemovalOrder(*app, 11);
    Check(history.PrepareNative("remove accessory", accessoryOrder, tx) == MMS_OK && history.CommitNative(std::move(tx)) == MMS_OK && later.order == 0,
        "capture accessory order before its peer pool grows");
    const auto orderIdentity = mdl::KeyPoolIdentity(app->AccessoryKeys(12));
    Check(mdl::KeyPoolEnsure(app->AccessoryKeys(12), 5) && mdl::KeyPoolIdentity(app->AccessoryKeys(12)) == orderIdentity,
        "surviving accessory pool grows independently");
    Check(history.History(false) == MMS_OK && later.order == 1, "accessory-order undo accepts registered peer pool growth");
    ForgetSceneObject(peer.raw()); SceneObjectIdentity(peer.raw());
    std::unique_ptr<PreparedHistoryChange> prepared;
    Check(change->Prepare(true, prepared) == MMS_E_STALE, "same-address object with new stable ID cannot revive binding history");
    history.Reset(); history.Collect(); change.reset(); accessoryOrder.reset();
    for (size_t i = 4; i < mdl::KeyPoolCapacity(peer.m->displayKeys); ++i) {
        std::free(peer.m->displayKeys[i].ikStates); std::free(peer.m->displayKeys[i].selectorStates);
    }
    peer.m->boneOrderTable = nullptr;
    ForgetSceneObject(peer.raw()); ForgetSceneObject(deleted.raw()); ForgetSceneObject(&accessory); ForgetSceneObject(&later);
    mdl::KeyPoolDestroy(app->CameraKeys()); mdl::KeyPoolDestroy(app->AccessoryKeys(11)); mdl::KeyPoolDestroy(app->AccessoryKeys(12));

    // Generic growth failure must leave both the old identity and bytes intact.
    auto* pool = mdl::KeyPoolCreate<mdl::CameraKey>(4); pool[2].parentBone = 67;
    const auto identity = mdl::KeyPoolIdentity(pool); auto* old = pool;
    for (long limit = 0; limit < 64; ++limit) {
        failAfter = limit; const auto ok = mdl::KeyPoolEnsure(pool, 5); failAfter = -1;
        if (ok) { Check(pool != old && mdl::KeyPoolIdentity(pool) == identity && pool[2].parentBone == 67, "generic growth commits the preserved identity"); break; }
        Check(pool == old && mdl::KeyPoolCapacity(pool) == 4 && mdl::KeyPoolIdentity(pool) == identity && pool[2].parentBone == 67,
            "failed generic growth preserves pointer capacity identity and contents");
        Check(limit < 63, "generic pool growth failure sweep terminates");
    }
    mdl::KeyPoolDestroy(pool);
}

void RemovalBindingsBorrowedPool() {
    using namespace plugins;
    Fixture deleted; auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
    app->ModelSlot(3) = deleted.raw(); app->CameraParentModel() = -1;
    std::vector<mdl::CameraKey> borrowed(mdl::KeyPoolStorage<mdl::CameraKey>::initial);
    for (auto& key : borrowed) key.parentModel = -1;
    borrowed[1].parentModel = 3; borrowed[1].parentBone = 71; app->CameraKeys() = borrowed.data();
    auto change = CaptureModelRemovalBindings(*app, 3); std::unique_ptr<PreparedHistoryChange> candidate;
    Check(!mdl::KeyPoolIdentity(borrowed.data()) && change->Prepare(true, candidate) == MMS_OK, "borrowed pool uses conservative original address");
    auto copy = borrowed; app->CameraKeys() = copy.data();
    Check(!candidate->IsCurrent() && change->Prepare(true, candidate) == MMS_E_STALE, "identical borrowed pool at another address is stale");
    app->CameraKeys() = borrowed.data();
    Check(mdl::KeyPoolEnsure(app->CameraKeys(), borrowed.size() + 1), "borrowed pool can grow into independently owned storage");
    Check(mdl::KeyPoolIdentity(app->CameraKeys()) && change->Prepare(true, candidate) == MMS_E_STALE && borrowed[1].parentModel == 3,
        "borrowed-to-owned growth does not claim an unregistered identity");
    candidate.reset(); change.reset(); mdl::KeyPoolDestroy(app->CameraKeys()); ForgetSceneObject(deleted.raw());
}

void RemovalBindingsBatch() {
    using namespace plugins;
    Fixture first, second, peer; auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
    app->ModelSlot(3) = first.raw(); app->ModelSlot(5) = second.raw(); app->ModelSlot(7) = peer.raw();
    first.m->comboSelIndex = 0; second.m->comboSelIndex = 1; peer.m->comboSelIndex = 2;
    first.m->comboSelIndex2 = 2; second.m->comboSelIndex2 = 0; peer.m->comboSelIndex2 = 1;
    mdl::BoneOrderEntry firstOrder{}, secondOrder{}, peerOrder{};
    firstOrder.linkedModel = 5; firstOrder.linkedBone = 79; secondOrder.linkedModel = 3; secondOrder.linkedBone = 83;
    peerOrder.linkedModel = 5; peerOrder.linkedBone = 89;
    first.m->boneOrderTable = &firstOrder; second.m->boneOrderTable = &secondOrder; peer.m->boneOrderTable = &peerOrder;
    for (unsigned i = 0; i < 4; ++i) peer.m->displayKeys[i].selectorStates[0] = {-1, 0};
    first.m->displayKeys[1].selectorStates[0] = {5, 97}; second.m->displayKeys[1].selectorStates[0] = {3, 101};
    peer.m->displayKeys[2].selectorStates[0] = {3, 103};
    app->CameraParentModel() = 5; app->CameraParentBone() = 107;
    app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4);
    for (unsigned i = 0; i < 4; ++i) app->CameraKeys()[i].parentModel = -1;
    app->CameraKeys()[1].parentModel = 3; app->CameraKeys()[2].parentModel = 5; app->CameraKeys()[3].parentModel = 7;
    mdl::AccessoryRecord a{}, b{}, c{}; a.order = 0; b.order = 1; c.order = 2;
    a.parentModel = 5; b.parentModel = 3; c.parentModel = 5; c.parentBone = 109;
    app->AccessorySlot(11) = &a; app->AccessorySlot(12) = &b; app->AccessorySlot(13) = &c; app->AccessoryRenderSplitOrder() = 2;
    for (unsigned slot : {11u, 12u, 13u}) {
        app->AccessoryKeys(slot) = mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
        for (unsigned i = 0; i < 4; ++i) app->AccessoryKeys(slot)[i].parentModel = -1;
        app->AccessoryKeys(slot)[1].parentModel = 3; app->AccessoryKeys(slot)[1].visible = 1;
    }
    const auto firstDigest = first.Digest(), secondDigest = second.Digest(), peerDigest = peer.Digest();
    const auto unchanged = [&] { return first.Digest() == firstDigest && second.Digest() == secondDigest && peer.Digest() == peerDigest &&
        peerOrder.linkedModel == 5 && app->CameraParentModel() == 5 && app->CameraKeys()[1].parentModel == 3 &&
        app->CameraKeys()[2].parentModel == 5 && c.order == 2 && c.parentModel == 5 && app->AccessoryRenderSplitOrder() == 2; };
    const auto rejects = [&](std::vector<unsigned> models, std::vector<unsigned> accessories) {
        try { CaptureObjectsRemovalBindings(*app, models, accessories); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    Check(rejects({}, {}) && rejects({3, 3}, {}) && rejects({}, {11, 11}) && rejects({255}, {}) && rejects({}, {255}) &&
        rejects({4}, {}) && rejects({}, {14}) && unchanged(), "batch removal rejects empty duplicate invalid and absent slots without mutations");
    for (long limit = 0; limit < 4096; ++limit) {
        std::shared_ptr<HistoryChange> captured; bool memory = false;
        failAfter = limit; try { captured = CaptureObjectsRemovalBindings(*app, {3, 5}, {11, 12}); } catch (const std::bad_alloc&) { memory = true; } failAfter = -1;
        Check(unchanged(), "batch binding capture allocation failure leaves every object unchanged");
        if (captured) break;
        Check(memory && limit < 4095, "batch capture failure sweep terminates");
    }
    auto change = CaptureObjectsRemovalBindings(*app, {3, 5}, {11, 12});
    StorageStore storage; SharedHistory history(storage); std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(history.PrepareNative("remove two models and accessories", change, tx) == MMS_OK, "prepare one batch binding record");
    failAfter = 0; const auto committed = history.CommitNative(std::move(tx)); failAfter = -1;
    Check(committed == MMS_OK && history.UndoCount() == 1 && peer.m->comboSelIndex == 0 && peer.m->comboSelIndex2 == 0 &&
        c.order == 0 && app->AccessoryRenderSplitOrder() == 0, "one batch subtracts all preceding removed objects without allocating");
    Check(first.Digest() == firstDigest && second.Digest() == secondDigest && firstOrder.linkedModel == 5 && secondOrder.linkedModel == 3 &&
        a.parentModel == 5 && b.parentModel == 3 && app->AccessoryKeys(11)[1].parentModel == 3 && app->AccessoryKeys(12)[1].parentModel == 3,
        "objects removed together retain their own cross-bindings and key values");
    Check(peerOrder.linkedModel == -1 && peer.m->displayKeys[2].selectorStates[0].modelIndex == -1 &&
        app->CameraParentModel() == -1 && app->CameraKeys()[1].parentModel == -1 && app->CameraKeys()[2].parentModel == -1 &&
        app->CameraKeys()[3].parentModel == 7 && c.parentModel == -1 && app->AccessoryKeys(13)[1].parentModel == -1,
        "batch clears surviving incoming bindings once and preserves unrelated targets");
    Check(history.History(false) == MMS_OK && unchanged(), "one undo restores batch order and bindings");
    Check(history.History(true) == MMS_OK && c.order == 0 && peer.m->comboSelIndex == 0, "one redo reapplies the batch delta");
    history.Reset(); history.Collect(); change.reset();
    first.m->boneOrderTable = second.m->boneOrderTable = peer.m->boneOrderTable = nullptr;
    ForgetSceneObject(first.raw()); ForgetSceneObject(second.raw()); ForgetSceneObject(peer.raw());
    ForgetSceneObject(&a); ForgetSceneObject(&b); ForgetSceneObject(&c);
    mdl::KeyPoolDestroy(app->CameraKeys());
    for (unsigned slot : {11u, 12u, 13u}) mdl::KeyPoolDestroy(app->AccessoryKeys(slot));
}
