// Public layout validation: menu overflow is restricted to toolbar mounts,
// including after edits, preference import, and a missing plugin's return.
static void OverflowValidationChecks() {
    UiRegistry registry; UiClient owner{&registry,"example.overflow.validation"};
    auto api=registry.Api(owner);auto layout=registry.LayoutApi(owner);
    MmsUiElementV1 separator{};separator.h=Header<MmsUiElementV1>();
    separator.id=S("example.overflow.validation.separator");separator.ownerPlugin=Text(owner.id);
    separator.parentId=S("mms.toolbar.main.extensions");separator.kind=MMS_UI_SEPARATOR;
    separator.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE;
    MmsHandle handle=0;CHECK(api.registerElement(api.context,&separator,&handle)==MMS_OK);
    MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.overflow=MMS_UI_OVERFLOW_MENU;
    CHECK(layout.setDefaultHints(layout.context,separator.id,&hints)==MMS_OK);
    const auto oldRevision=registry.Revision(),oldGeneration=registry.elements.at(String(separator.id)).desc.generation;
    const auto oldPreferences=registry.ExportPreferences();
    auto moved=separator;moved.parentId=S("mms.menu.plugins");
    CHECK(api.updateElement(api.context,handle,&moved)==MMS_E_UNSUPPORTED);
    CHECK(registry.elements.at(String(separator.id)).handle==handle && registry.Revision()==oldRevision && registry.ExportPreferences()==oldPreferences);
    CHECK(registry.elements.at(String(separator.id)).desc.generation==oldGeneration &&
        String(registry.elements.at(String(separator.id)).desc.parentId)=="mms.toolbar.main.extensions");
    MmsHandle transaction=0;CHECK(api.beginLayout(api.context,registry.Revision(),&transaction)==MMS_OK);
    MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_MOVE;op.elementId=separator.id;
    op.parentId=S("mms.menu.plugins");op.expectedGeneration=oldGeneration;
    CHECK(api.applyLayout(api.context,transaction,&op)==MMS_OK);
    CHECK(api.commitLayout(api.context,transaction)==MMS_E_UNSUPPORTED);
    CHECK(registry.Revision()==oldRevision && registry.ExportPreferences()==oldPreferences);
    api.cancelLayout(api.context,transaction);
    const auto invalid=std::string("{\"schemaVersion\":1,\"elements\":[{\"id\":\"example.overflow.validation.separator\",\"owner\":\"example.overflow.validation\",\"kind\":7,\"parent\":\"mms.menu.plugins\",\"hints\":{\"overflow\":3}}]}");
    CHECK(registry.ImportPreferences(invalid)==MMS_E_UNSUPPORTED);
    CHECK(registry.Revision()==oldRevision && registry.ExportPreferences()==oldPreferences);
    hints.overflow=0;CHECK(layout.setDefaultHints(layout.context,separator.id,&hints)==MMS_OK);
    CHECK(api.updateElement(api.context,handle,&moved)==MMS_OK);
    CHECK(registry.elements.at(String(separator.id)).handle==handle);
    const auto menuRevision=registry.Revision();hints.overflow=MMS_UI_OVERFLOW_MENU;
    CHECK(layout.setDefaultHints(layout.context,separator.id,&hints)==MMS_E_UNSUPPORTED);
    CHECK(registry.Revision()==menuRevision);
    CHECK(api.beginLayout(api.context,registry.Revision(),&transaction)==MMS_OK);
    CHECK(layout.stageHints(layout.context,transaction,separator.id,
        registry.elements.at(String(separator.id)).desc.generation,&hints)==MMS_OK);
    CHECK(api.commitLayout(api.context,transaction)==MMS_E_UNSUPPORTED);
    CHECK(registry.Revision()==menuRevision);api.cancelLayout(api.context,transaction);
    const std::string missing="{\"schemaVersion\":1,\"elements\":[{\"id\":\"example.returned.separator\",\"owner\":\"example.returned\",\"kind\":7,\"parent\":\"mms.menu.plugins\",\"hints\":{\"overflow\":3}}]}";
    CHECK(registry.ImportPreferences(missing)==MMS_OK);
    const auto retained=registry.ExportPreferences();UiClient returned{&registry,"example.returned"};auto returnedApi=registry.Api(returned);
    auto returnedElement=separator;returnedElement.id=S("example.returned.separator");returnedElement.ownerPlugin=Text(returned.id);
    CHECK(returnedApi.registerElement(returnedApi.context,&returnedElement,&handle)==MMS_OK);
    CHECK(registry.elements.at("example.returned.separator").effectiveHints.overflow==0);
    CHECK(registry.ExportPreferences()==retained);
    registry.Retire(returned);registry.Retire(owner);

    // A saved missing group can have an invalid parent. Restore the group
    // before validating its alphabetically earlier child's menu preference.
    UiRegistry grouped;UiClient groupOwner{&grouped,"example.groupreturn"};
    auto groupApi=grouped.Api(groupOwner);auto groupLayout=grouped.LayoutApi(groupOwner);
    const std::string groupPreferences=R"({"schemaVersion":1,"elements":[{"id":"example.groupreturn.zgroup","owner":"example.groupreturn","kind":8,"parent":"mms.menu.plugins"},{"id":"example.groupreturn.aseparator","owner":"example.groupreturn","kind":7,"hints":{"overflow":3}}]})";
    CHECK(grouped.ImportPreferences(groupPreferences)==MMS_OK);
    MmsUiElementV1 group{};group.h=Header<MmsUiElementV1>();group.id=S("example.groupreturn.zgroup");
    group.ownerPlugin=Text(groupOwner.id);group.kind=MMS_UI_GROUP;group.parentId=S("mms.toolbar.main.extensions");
    group.capabilities=MMS_UI_CONTAINER|MMS_UI_LAYOUT|MMS_UI_MOVE;
    CHECK(groupApi.registerElement(groupApi.context,&group,&handle)==MMS_OK);
    auto child=separator;child.id=S("example.groupreturn.aseparator");child.ownerPlugin=Text(groupOwner.id);
    child.parentId=group.id;
    CHECK(groupApi.registerElement(groupApi.context,&child,&handle)==MMS_OK);
    CHECK(String(grouped.elements.at("example.groupreturn.zgroup").desc.parentId)=="mms.toolbar.main.extensions");
    CHECK(grouped.elements.at("example.groupreturn.aseparator").effectiveHints.overflow==MMS_UI_OVERFLOW_MENU);
    hints.overflow=MMS_UI_OVERFLOW_MENU;
    CHECK(groupLayout.setDefaultHints(groupLayout.context,child.id,&hints)==MMS_OK);
    auto extra=separator;extra.id=S("example.groupreturn.extra");extra.ownerPlugin=Text(groupOwner.id);
    CHECK(groupApi.registerElement(groupApi.context,&extra,&handle)==MMS_OK);
    CHECK(String(grouped.elements.at("example.groupreturn.aseparator").desc.parentId)=="example.groupreturn.zgroup");
    CHECK(grouped.elements.at("example.groupreturn.aseparator").effectiveHints.overflow==MMS_UI_OVERFLOW_MENU);
    grouped.Retire(groupOwner);
}
