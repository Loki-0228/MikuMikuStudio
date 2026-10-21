#pragma once

// A native call can synchronously destroy either window before it returns.
// Pins do not allocate; retirement removes future entry points, and the last
// active callback destroys the detached State. HWND reuse is checked through
// the exact existing subclass/refData pair, not IsWindow alone.
namespace curve_editor_control_lifetime {
struct State {
    unsigned callbacks = 0;
    bool retired = false;
};
template<class T> class Pin {
    T* state_;
public:
    explicit Pin(T& value) noexcept : state_(&value) { ++state_->callbacks; }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    ~Pin() { if (!--state_->callbacks && state_->retired) delete state_; }
};
inline bool Exact(HWND window, SUBCLASSPROC procedure, UINT_PTR id, const void* state) noexcept {
    DWORD_PTR current = 0;
    return window && GetWindowSubclass(window, procedure, id, &current) &&
        current == reinterpret_cast<DWORD_PTR>(state);
}
struct Paint {
    HWND window;
    PAINTSTRUCT paint{};
    HDC dc;
    explicit Paint(HWND value) noexcept : window(value), dc(BeginPaint(value, &paint)) {}
    ~Paint() { EndPaint(window, &paint); }
};
struct Dc {
    HDC dc; int saved;
    explicit Dc(HDC value) noexcept : dc(value), saved(SaveDC(value)) {}
    ~Dc() { if (saved) RestoreDC(dc, saved); }
};
}
