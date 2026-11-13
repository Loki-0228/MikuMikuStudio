static void SceneChecks() {
    SceneIdentity identities; int token = 0;
    const auto original = identities.Get(&token).object;
    const auto boneId = identities.Element(&token, MMS_CHANNEL_BONES, 0);
    CHECK(!Empty(original.id) && original.generation && !Empty(boneId));
    CHECK(Same(original, identities.Get(&token).object));
    auto saved = identities.Get(&token);
    identities.Forget(&token); CHECK(identities.Known(original.id));
    const auto replacement = identities.Get(&token).object;
    CHECK(!Same(original.id, replacement.id) && original.generation != replacement.generation);
    identities.Replace({{&token, saved}});
    CHECK(Same(identities.Get(&token).object.id, original.id));
    {
        SceneIdentity adapted; int changed = 0, unchanged = 0;
        const auto changedObject = adapted.Get(&changed).object;
        const auto unchangedObject = adapted.Get(&unchanged).object;
        const auto changedBone = adapted.Element(&changed, MMS_CHANNEL_BONES, 0);
        const auto changedVertex = adapted.Element(&changed, MMS_CHANNEL_MESH, 2);
        const auto unchangedBone = adapted.Element(&unchanged, MMS_CHANNEL_BONES, 0);
        const auto changedEntry = adapted.Get(&changed), unchangedEntry = adapted.Get(&unchanged);
        adapted.Replace({{&changed, changedEntry, false}, {&unchanged, unchangedEntry}});
        CHECK(Same(adapted.Get(&changed).object.id, changedObject.id));
        CHECK(adapted.Get(&changed).object.generation != changedObject.generation);
        CHECK(adapted.Get(&changed).elements.empty());
        CHECK(adapted.Known(changedBone) && adapted.Known(changedVertex));
        CHECK(!Same(adapted.Element(&changed, MMS_CHANNEL_BONES, 0), changedBone));
        CHECK(!Same(adapted.Element(&changed, MMS_CHANNEL_MESH, 2), changedVertex));
        CHECK(Same(adapted.Get(&unchanged).object.id, unchangedObject.id));
        CHECK(Same(adapted.Element(&unchanged, MMS_CHANNEL_BONES, 0), unchangedBone));
        const auto settled = adapted.Get(&changed);
        const auto unchangedIdentity = [&] {
            const auto& current = adapted.Get(&changed);
            return Same(current.object, settled.object) &&
                std::equal(current.elements.begin(), current.elements.end(),
                    settled.elements.begin(), settled.elements.end(), [](const auto& a, const auto& b) {
                        return a.first == b.first && Same(a.second, b.second);
                    });
        };
        auto bad = changedEntry;
        bad.elements.begin()->second = unchangedBone;
        CHECK(Throws([&] { adapted.Replace({{&changed, bad, false}, {&unchanged, unchangedEntry}}); }));
        CHECK(unchangedIdentity());
        bad = changedEntry; bad.elements.begin()->second = {};
        CHECK(Throws([&] { adapted.Replace({{&changed, bad, false}}); }));
        CHECK(unchangedIdentity());
    }
    CHECK(identities.Get(&token).object.generation != original.generation);
    CHECK(Same(identities.Element(&token, MMS_CHANNEL_BONES, 0), boneId));
    CHECK(Throws([&] { identities.Replace({{&token, saved},{&token, saved}}); }));
    CHECK(Same(identities.Get(&token).object.id, original.id));
    const auto makeData = [&](const char* name) {
        auto data = std::make_shared<SceneData>(); data->time = {1,30}; data->supported = MMS_CHANNEL_BONES;
        SceneData::Channels object; object.object.h = Header<MmsObjectV1>(); object.object.object = identities.Get(&token).object;
        object.object.name = data->Keep(std::string(name));
        MmsBoneV1 bone{}; bone.h = Header<MmsBoneV1>(); bone.id = boneId; bone.name = data->Keep(std::string("骨骼"));
        object.values[MMS_CHANNEL_BONES] = data->Keep(std::vector<MmsBoneV1>{bone});
        data->objects.push_back(object); return data;
    };
    auto current = makeData("旧模型");
    MmsSceneApiV1 api{}; bool nested = false; MmsResult nestedResult = MMS_OK;
    SceneStore store([&](const MmsSceneQueryV1& query, auto& out) {
        if(nested) { MmsSnapshotV1 ignored{}; nestedResult = api.acquire(api.context,&query,&ignored); }
        out = current; return MMS_OK;
    }, [&](MmsId id) { return identities.Known(id); });
    SceneClient client, stranger; MmsSceneApiV1 other{};
    CHECK(store.Api(client, api) == MMS_OK && store.Api(stranger, other) == MMS_OK);
    MmsSceneQueryV1 query{}; query.h = Header<MmsSceneQueryV1>(); query.time = {2,60}; query.channels = MMS_CHANNEL_BONES;
    MmsSnapshotV1 old{};
    CHECK(api.acquire(api.context,&query,&old)==MMS_OK && store.HasLeases(client));
    auto now=query;now.flags=MMS_SCENE_CURRENT_TIME;now.time={};MmsSnapshotV1 latest{};
    CHECK(api.acquire(api.context,&now,&latest)==MMS_OK);api.release(api.context,latest.handle);
    now.flags=2;CHECK(api.acquire(api.context,&now,&latest)==MMS_E_ARGUMENT);
    MmsSpan objects{}, bones{};
    CHECK(api.objects(api.context,old.handle,&objects)==MMS_OK && objects.count==1);
    const auto* oldObject = static_cast<const MmsObjectV1*>(objects.data);
    CHECK(api.readChannel(api.context,old.handle,oldObject->object,MMS_CHANNEL_BONES,&bones)==MMS_OK && bones.count==1);
    CHECK(other.objects(other.context,old.handle,&objects)==MMS_E_STALE);
    other.release(other.context,old.handle); CHECK(store.HasLeases(client));
    current = makeData("新模型"); current->revision = 2;
    CHECK(String(oldObject->name)=="旧模型" && String(static_cast<const MmsBoneV1*>(bones.data)->name)=="骨骼");
    MmsSnapshotV1 newer{}; nested=true;
    CHECK(api.acquire(api.context,&query,&newer)==MMS_OK && nestedResult==MMS_E_BUSY); nested=false;
    CHECK(newer.sceneRevision==2 && old.sceneRevision==1);
    query.time={2,30}; CHECK(api.acquire(api.context,&query,&newer)==MMS_E_NOT_READY);
    query.time={0,0}; CHECK(api.acquire(api.context,&query,&newer)==MMS_E_ARGUMENT);
    query.time={1,30}; query.channels=MMS_CHANNEL_MESH;
    CHECK(api.acquire(api.context,&query,&newer)==MMS_E_UNSUPPORTED);
    query.channels=MMS_CHANNEL_BONES;
    CHECK(api.readChannel(api.context,old.handle,oldObject->object,3,&bones)==MMS_E_ARGUMENT);
    auto stale = oldObject->object; ++stale.generation;
    query.objects={&stale,1,sizeof(stale),1}; CHECK(api.acquire(api.context,&query,&newer)==MMS_E_STALE);
    query.objects={&oldObject->object,1,sizeof(stale),1}; current->objects.clear();
    CHECK(api.acquire(api.context,&query,&newer)==MMS_E_STALE);
    query.objects={};
    MmsResult workerAcquire=MMS_OK, workerRead=MMS_E_FAILED;
    std::thread worker([&] { MmsSnapshotV1 ignored{}; MmsSpan records{};
        workerAcquire=api.acquire(api.context,&query,&ignored);
        workerRead=api.objects(api.context,old.handle,&records);
        api.release(api.context,old.handle);
    }); worker.join();
    CHECK(workerAcquire==MMS_E_THREAD && workerRead==MMS_OK);
    CHECK(api.objects(api.context,old.handle,&objects)==MMS_E_STALE);
    api.release(api.context,newer.handle); CHECK(!store.HasLeases(client));
    client.stopping=true; CHECK(api.acquire(api.context,&query,&newer)==MMS_E_NOT_READY);
    CHECK(Normalize({INT64_MIN,uint64_t(1)<<63}).numerator==-1);
    CHECK(Normalize({0,UINT64_MAX}).denominator==1);
}
