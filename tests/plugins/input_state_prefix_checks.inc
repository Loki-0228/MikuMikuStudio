// Include after input_state_content_checks.inc. The old ABI structure ends at
// the last readable byte, so any attempted extension read hits PAGE_NOACCESS.
#include <new>
namespace input_state_prefix_checks {
struct ReleasePages {
    void operator()(unsigned char* value) const noexcept { if (value) VirtualFree(value, 0, MEM_RELEASE); }
};
struct GuardedPrefix {
    std::unique_ptr<unsigned char, ReleasePages> pages;
    MmsUiStateContentV1* content = nullptr;
    GuardedPrefix() {
        static_assert(sizeof(MmsUiStateContentV1) == 64);
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        const auto page = size_t(system.dwPageSize); CHECK(page >= sizeof(MmsUiStateContentV1));
        pages.reset(static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)));
        CHECK(pages);
        DWORD previous = 0; auto* const guard = pages.get() + page;
        CHECK(VirtualProtect(guard, page, PAGE_NOACCESS, &previous) && previous == PAGE_READWRITE);
        MEMORY_BASIC_INFORMATION information{};
        CHECK(VirtualQuery(guard, &information, sizeof(information)) == sizeof(information) &&
            information.State == MEM_COMMIT && information.Protect == PAGE_NOACCESS);
        auto* const address = guard - sizeof(MmsUiStateContentV1);
        CHECK(reinterpret_cast<uintptr_t>(address) % alignof(MmsUiStateContentV1) == 0);
        content = ::new (static_cast<void*>(address)) MmsUiStateContentV1{};
        CHECK(reinterpret_cast<unsigned char*>(content) + sizeof(*content) == guard);
        content->h = Header<MmsUiStateContentV1>();
    }
};
std::pair<DWORD, DWORD> Selection(HWND field) {
    DWORD begin = 0, end = 0;
    SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
    return {begin, end};
}
void LegacyAndShortExtension() {
    GuardedPrefix prefix; input_state_content_checks::Fixture f; f.Register();
    const auto field = f.field, wrapper = f.wrapper, label = f.labelWindow;
    input_state_content_checks::SetRaw(field, L"7"); SendMessageW(field, EM_SETSEL, 0, 1);
    const auto selection = Selection(field); const auto reads = f.data.reads, releases = f.data.releases;
    uint64_t revision = 0; CHECK(f.history.Revision(revision) == MMS_OK);
    auto& content = *prefix.content; content.fields = MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION | MMS_UI_STATE_DISABLED_REASON;
    f.label = "Legacy content at the readable page boundary";
    content.label = Text(f.label); content.description = Text(f.description); content.disabledReason = Text(f.reason);
    MmsResult written = MMS_E_FAILED;
    f.prepare = [&](MmsHandle builder) {
        written = f.content.writePrepared(f.content.context, builder, prefix.content); return written;
    };
    const auto legacy = f.Submit(); f.queue.Drain(); f.Complete(legacy);
    CHECK(written == MMS_OK && native_property_input_checks::Caption(label) == Wide(f.label));
    CHECK(f.field == field && GetParent(field) == wrapper && native_property_input_checks::FindField(wrapper) == field &&
        native_property_input_checks::Caption(field) == L"7" && GetFocus() == field && Selection(field) == selection);
    CHECK(f.data.reads == reads && f.data.releases == releases && !(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY));
    const auto previousLabel = native_property_input_checks::Caption(label), previousHelp = f.Help(); f.Clean();

    // This is a truthful 64-byte extent with an unsupported short extension,
    // not an invalid pointer falsely claiming an 80-byte allocation.
    f.label = "Rejected short INPUT extension"; content.label = Text(f.label); content.fields |= MMS_UI_STATE_READ_ONLY;
    CHECK(content.h.size == 64);
    const auto shortInput = f.Submit(); f.queue.Drain(); const auto rejected = f.State(shortInput);
    CHECK(written == MMS_E_ARGUMENT && rejected.status == MMS_UI_REFRESH_FAILED && rejected.result == MMS_E_ARGUMENT &&
        !rejected.completedKinds && !rejected.sourceRevision);
    CHECK(native_property_input_checks::Caption(label) == previousLabel && f.Help() == previousHelp &&
        native_property_input_checks::Caption(field) == L"7" && Selection(field) == selection && GetFocus() == field &&
        !(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY));
    CHECK(f.data.reads == reads && f.data.releases == releases && f.field == field && GetParent(field) == wrapper); f.Clean();

    content.fields &= ~MMS_UI_STATE_READ_ONLY; f.label = "Legacy builder recovers after the short extension"; content.label = Text(f.label);
    const auto recovered = f.Submit(); f.queue.Drain(); f.Complete(recovered);
    CHECK(written == MMS_OK && native_property_input_checks::Caption(label) == Wide(f.label) &&
        native_property_input_checks::Caption(field) == L"7" && Selection(field) == selection);
    uint64_t current = 0; CHECK(f.history.Revision(current) == MMS_OK && current == revision && f.data.reads == reads && f.data.releases == releases);
    native_property_input_checks::Escape(field); CHECK(native_property_input_checks::Caption(field) == L"2"); f.Clean();
}
} // namespace input_state_prefix_checks
static void InputStatePrefixChecks() {
    input_state_prefix_checks::LegacyAndShortExtension();
    std::cout << "INPUT STATE guarded 64-byte prefix/short extension/no tail read PASS\n";
}
