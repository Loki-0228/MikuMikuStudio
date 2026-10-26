#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mms/surface.h>
#include <mms/cpp/image_canvas.hpp>
#include <climits>
#include <cwchar>
#include <utility>

namespace mms::sdk {
// Single-view surface factory using the original surface and CPU image tables.
// Keep this object at a stable address until its surface has been unregistered.
// Unregister the panel element and surface, then Close before destroying it.
// No host calls occur in the destructor; plugin shutdown owns that ordering.
class ImagePanel {
    MmsRenderImageApiV1 api_{};
    MmsRenderImageInfoV1 info_{};
    MmsRenderImageMapV1 map_{};
    MmsHandle pending_ = 0;
    ImageCanvasOptions options_{};
    CpuImageCanvas canvas_;
    DWORD thread_ = 0;
    HWND window_ = nullptr;
    HINSTANCE module_ = nullptr;
    ATOM atom_ = 0;
    wchar_t className_[96]{};
    MmsResult last_ = MMS_OK;

    template<class T> static MmsHeader Header() noexcept { return {sizeof(T), 1, 0}; }
    MmsResult Check() const noexcept {
        if (!thread_) return MMS_E_NOT_READY;
        return GetCurrentThreadId() == thread_ ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult Unmap(MmsHandle mapping) noexcept {
        if (!mapping) return MMS_OK;
        const auto result = api_.unmap(api_.context, mapping);
        return result == MMS_E_NOT_FOUND ? MMS_OK : result;
    }
    MmsResult DrainPending() noexcept {
        const auto result = Unmap(pending_);
        if (result == MMS_OK) pending_ = 0;
        return result;
    }
    void Discard(MmsHandle mapping) noexcept {
        // Valid mappings in the host 1.0 implementation release without an
        // allocation. Preserve a failed cleanup token for explicit retry.
        if (Unmap(mapping) != MMS_OK) pending_ = mapping;
    }
    void Size(uint32_t& width, uint32_t& height) const noexcept {
        RECT rect{};
        if (window_ && GetClientRect(window_, &rect)) {
            width = static_cast<uint32_t>(rect.right);
            height = static_cast<uint32_t>(rect.bottom);
        } else width = height = 0;
    }
    MmsResult Prepare(CpuImageCanvas& candidate, const MmsRenderImageInfoV1* info,
                      const MmsRenderImageMapV1* map, ImageCanvasOptions options) const noexcept {
        uint32_t width = 0, height = 0; Size(width, height);
        return candidate.Compose(info, map, width, height, options);
    }
    void Rebuild() noexcept {
        last_ = Prepare(canvas_, map_.mapping ? &info_ : nullptr, map_.mapping ? &map_ : nullptr, options_);
    }
    void Paint(HDC dc) noexcept {
        if (!dc) return;
        uint32_t width = 0, height = 0; Size(width, height);
        if (!width || !height) return;
        if (canvas_.Width() == width && canvas_.Height() == height && canvas_.Pixels()) {
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
            bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            if (!SetDIBitsToDevice(dc, 0, 0, width, height, 0, 0, 0, height,
                                  canvas_.Pixels(), &bitmap, DIB_RGB_COLORS)) last_ = MMS_E_FAILED;
        } else {
            // A resize allocation failure must not leave old pixels uncovered.
            RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            const auto brush = CreateSolidBrush(RGB((options_.background >> 16) & 255,
                (options_.background >> 8) & 255, options_.background & 255));
            if (brush) { FillRect(dc, &rect, brush); DeleteObject(brush); }
            else last_ = MMS_E_MEMORY;
        }
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<ImagePanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ImagePanel*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            if (!self) return FALSE;
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message == WM_SIZE) { self->Rebuild(); InvalidateRect(window, nullptr, FALSE); return 0; }
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
                self->Paint(dc); EndPaint(window, &paint); return 0;
            }
            if (message == WM_PRINTCLIENT) { self->Paint(reinterpret_cast<HDC>(wparam)); return 0; }
            if (message == WM_NCDESTROY) {
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->window_ = nullptr;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    MmsResult Create(const MmsUiMountV1* mount, void** outView, uintptr_t* outWindow) noexcept {
        if (const auto result = Check()) return result;
        if (!mount || mount->h.size < sizeof(*mount) || mount->h.major != 1 || mount->h.minor ||
            !outView || !outWindow || mount->reserved || !mount->dpi) return MMS_E_ARGUMENT;
        if (window_) return MMS_E_BUSY;
        const HWND parent = reinterpret_cast<HWND>(mount->parentWindow);
        if (!IsWindow(parent)) return MMS_E_ARGUMENT;
        if (GetWindowThreadProcessId(parent, nullptr) != thread_) return MMS_E_THREAD;
        if (mount->bounds.width > INT_MAX || mount->bounds.height > INT_MAX) return MMS_E_ARGUMENT;
        if (!atom_) {
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&WindowProc), &module_)) return MMS_E_FAILED;
            swprintf_s(className_, L"MmsSdkImagePanel-%p-%p", static_cast<void*>(module_), static_cast<void*>(this));
            WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WindowProc;
            wc.hInstance = module_; wc.lpszClassName = className_;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW, also without UNICODE
            atom_ = RegisterClassExW(&wc);
            if (!atom_) return MMS_E_FAILED;
        }
        const auto window = CreateWindowExW(0, className_, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP,
            mount->bounds.x, mount->bounds.y, static_cast<int>(mount->bounds.width),
            static_cast<int>(mount->bounds.height), parent, nullptr, module_, this);
        if (!window) return MMS_E_FAILED;
        // WM_SIZE normally prepared the first buffer during CreateWindowEx.
        // Avoid a second full allocation/composition when it already matches.
        uint32_t width = 0, height = 0; Size(width, height);
        if (last_ != MMS_OK || canvas_.Width() != width || canvas_.Height() != height) Rebuild();
        if (last_ != MMS_OK) { const auto result = last_; DestroyWindow(window); return result; }
        *outView = this; *outWindow = reinterpret_cast<uintptr_t>(window); return MMS_OK;
    }
public:
    ImagePanel() = default;
    ImagePanel(const ImagePanel&) = delete;
    ImagePanel& operator=(const ImagePanel&) = delete;
    ImagePanel(ImagePanel&&) = delete;
    ImagePanel& operator=(ImagePanel&&) = delete;
    ~ImagePanel() = default;

    MmsResult Initialize(const MmsRenderImageApiV1& api) noexcept {
        if (thread_) return MMS_E_BUSY;
        if (api.h.size < sizeof(api) || api.h.major != 1 || api.h.minor) return MMS_E_VERSION;
        if (!api.context || !api.describe || !api.mapRead || !api.unmap) return MMS_E_ARGUMENT;
        api_ = api; thread_ = GetCurrentThreadId(); last_ = MMS_OK; return MMS_OK;
    }
    MmsResult Initialize(const MmsHostV1& host) noexcept {
        if (thread_) return MMS_E_BUSY;
        if (!host.queryService) return MMS_E_ARGUMENT;
        MmsRenderImageApiV1 api{};
        constexpr char name[] = "mms.render.image";
        const auto result = host.queryService(host.context, {name, sizeof(name) - 1}, 1, 0, sizeof(api), &api);
        return result == MMS_OK ? Initialize(api) : result;
    }
    MmsUiSurfaceDescV1 Surface(MmsString id) noexcept {
        MmsUiSurfaceDescV1 desc{}; desc.h = Header<MmsUiSurfaceDescV1>(); desc.id = id; desc.context = this;
        desc.minimumWidth = 64; desc.minimumHeight = 36; desc.preferredWidth = 320; desc.preferredHeight = 180;
        desc.createView = [](void* self, const MmsUiMountV1* mount, void** view, uintptr_t* window) {
            return static_cast<ImagePanel*>(self)->Create(mount, view, window);
        };
        desc.resizeView = [](void* self, const MmsUiRectV1* bounds, uint32_t) {
            auto& panel = *static_cast<ImagePanel*>(self);
            if (const auto result = panel.Check()) { panel.last_ = result; return; }
            if (!bounds || bounds->width > INT_MAX || bounds->height > INT_MAX) { panel.last_ = MMS_E_ARGUMENT; return; }
            if (!panel.window_) { panel.last_ = MMS_E_NOT_READY; return; }
            if (!SetWindowPos(panel.window_, nullptr, bounds->x, bounds->y, static_cast<int>(bounds->width),
                    static_cast<int>(bounds->height), SWP_NOZORDER | SWP_NOACTIVATE)) panel.last_ = MMS_E_FAILED;
        };
        desc.showView = [](void* self, uint32_t visible) {
            auto& panel = *static_cast<ImagePanel*>(self);
            if (panel.Check() == MMS_OK && panel.window_) ShowWindow(panel.window_, visible ? SW_SHOWNA : SW_HIDE);
        };
        desc.focusView = [](void* self) -> MmsResult {
            auto& panel = *static_cast<ImagePanel*>(self);
            if (const auto result = panel.Check()) return result;
            if (!panel.window_ || !IsWindowVisible(panel.window_)) return MMS_E_NOT_READY;
            SetFocus(panel.window_); return GetFocus() == panel.window_ ? MMS_OK : MMS_E_FAILED;
        };
        desc.destroyView = [](void* self) {
            auto& panel = *static_cast<ImagePanel*>(self);
            if (panel.Check() == MMS_OK && panel.window_ && !DestroyWindow(panel.window_)) panel.last_ = MMS_E_FAILED;
        };
        return desc;
    }
    // Map first and prepare the complete candidate before replacing the current
    // image. The caller may release its image handle after this succeeds.
    MmsResult SetImage(MmsHandle image) noexcept {
        if (const auto result = Check()) return result;
        if (const auto result = DrainPending()) return result;
        MmsRenderImageInfoV1 info{}; MmsRenderImageMapV1 mapped{};
        if (image) {
            if (const auto result = api_.describe(api_.context, image, &info)) return result;
            if (const auto result = api_.mapRead(api_.context, image, &mapped)) return result;
        }
        CpuImageCanvas candidate;
        const auto result = Prepare(candidate, image ? &info : nullptr, image ? &mapped : nullptr, options_);
        if (result != MMS_OK) { Discard(mapped.mapping); return result; }
        if (const auto cleanup = Unmap(map_.mapping)) { Discard(mapped.mapping); return cleanup; }
        info_ = info; map_ = mapped; canvas_ = std::move(candidate); last_ = MMS_OK;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return MMS_OK;
    }
    MmsResult SetOptions(ImageCanvasOptions options) noexcept {
        if (const auto result = Check()) return result;
        CpuImageCanvas candidate;
        if (const auto result = Prepare(candidate, map_.mapping ? &info_ : nullptr, map_.mapping ? &map_ : nullptr, options)) return result;
        options_ = options; canvas_ = std::move(candidate); last_ = MMS_OK;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
        return MMS_OK;
    }
    // Close after unregistering the element and surface. An existing view is
    // never destroyed behind the host registry's back.
    MmsResult Close() noexcept {
        if (!thread_) return MMS_OK;
        if (const auto result = Check()) return result;
        if (window_) return MMS_E_BUSY;
        if (const auto result = DrainPending()) return result;
        if (const auto result = Unmap(map_.mapping)) return result;
        map_ = {}; info_ = {};
        if (atom_ && !UnregisterClassW(className_, module_)) return MMS_E_BUSY;
        atom_ = 0; module_ = nullptr; api_ = {}; thread_ = 0;
        canvas_ = CpuImageCanvas{}; return MMS_OK;
    }
    HWND Window() const noexcept { return window_; }
    MmsResult LastResult() const noexcept { return last_; }
};
} // namespace mms::sdk
