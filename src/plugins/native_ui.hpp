#pragma once
#include "ui_registry.hpp"

namespace mikumikustudio::plugins {
struct LayoutReservations {
    int toolbarHeight = 0, propertyHeight = 0, leftWidth = 0, rightWidth = 0;
};
class NativeUi {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeUi(UiRegistry&, HWND main, Log);
    ~NativeUi();
    void Refresh();
    // Compares the live command states with the last observation and reports a
    // single change per call. The host wires this to the COMMAND_STATE_CHANGED
    // business event; it is never called from plugin callbacks.
    void SetCommandStateChanged(std::function<void(uint64_t)>);
    void DetectCommandStateChanges();
    bool Command(unsigned id);
    bool Shortcut(unsigned key, unsigned modifiers, bool viewportFocused);
    // Returns reserved height. Coordinates use the parent window's DPI space.
    int Layout(int x, int width, int maximumHeight, bool visible);
    // Configure the real strip immediately above the native property panels.
    // bottom is in main-window coordinates; zero capacity reserves no space.
    // The next ordinary/queued layout publishes both mounts together.
    void SetPropertyArea(int bottom, int maximumHeight, bool visible);
    int PropertyAreaHeight() const noexcept;
    HWND PropertyArea() const noexcept;
    // Main-window coordinates. Each side measures its original retained tree;
    // an empty tree reserves zero width. Configure before the next Layout.
    void SetSideAreas(RECT bounds, int maximumWidth, bool visible);
    int LeftAreaWidth() const noexcept;
    int RightAreaWidth() const noexcept;
    HWND SideArea(bool right) const noexcept;
    // The refresh LAYOUT path calls this before publishing ready geometry;
    // the application updates dependent viewport geometry without scene work.
    void SetLayoutPublished(std::function<MmsResult(const LayoutReservations&)>);
    // Existing internal callers only consume the toolbar/workspace height.
    void SetLayoutPublished(std::function<MmsResult(int)>);
    HWND Toolbar() const;
    // Native painting/layout and popup tracking own adapter stacks independently
    // of plugin callback permissions. DeferClose coalesces requests while those
    // stacks are active, then posts one WM_CLOSE after the outermost return.
    // PopupActive reports popup tracking only, not general native work.
    bool PopupActive() const noexcept;
    // Read-only safe-point query; unlike DeferClose it does not post a close.
    bool Busy() const noexcept;
    bool DeferClose(WPARAM, LPARAM) noexcept;
    bool DialogMessage(MSG*);
};
}
