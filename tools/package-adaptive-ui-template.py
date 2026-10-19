#!/usr/bin/env python3
"""Package native_ui and standalone Win32 examples (Python 3.10+).

Run from any directory. --output selects a child of this checkout's
build/packages; --refresh updates only an existing package with the exact
expected file set. Build products remain outside the source archive.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


CMAKE = r'''cmake_minimum_required(VERSION 3.21)
project(mms_adaptive_ui_template VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This Win32 template requires MSVC on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure the x64 platform: -A x64")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
add_compile_options(/utf-8 /W4 /permissive-)
add_subdirectory(third_party/native_ui)

foreach(example IN ITEMS native_form viewport_host)
    add_executable(${example} WIN32 third_party/native_ui/examples/form.cpp)
    target_link_libraries(${example} PRIVATE native_ui::win32)
    target_link_options(${example} PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
endforeach()
target_compile_definitions(viewport_host PRIVATE NATIVE_UI_SIMULATED_VIEWPORT=1)

add_executable(adaptive_workspace WIN32 examples/workspace.cpp examples/app.manifest)
target_link_libraries(adaptive_workspace PRIVATE native_ui::win32)
target_compile_definitions(adaptive_workspace PRIVATE TEMPLATE_VIEWPORT=1)
target_link_options(adaptive_workspace PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)

include(CTest)
if(BUILD_TESTING)
    add_executable(layout_checks tests/layout_checks.cpp)
    target_link_libraries(layout_checks PRIVATE native_ui::layout native_ui::update)
    target_link_options(layout_checks PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
    add_test(NAME layout_checks COMMAND layout_checks)
    add_test(NAME native_form_hidden COMMAND native_form --self-test)
    add_test(NAME viewport_host_hidden COMMAND viewport_host --self-test)
    add_test(NAME adaptive_workspace_hidden COMMAND adaptive_workspace --self-test)
endif()
'''

MANIFEST = r'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="MMS.AdaptiveUi.Template" type="win32"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
  </application></compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
  </windowsSettings></application>
</assembly>
'''

WORKSPACE = r'''#include <native_ui/layout.hpp>
#include <native_ui/win32_host.hpp>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr UINT kApply = 1001, kReset = 1002, kRefresh = 1003;
struct Command { UINT id; std::wstring label; std::function<void()> invoke; };
struct Workspace {
    HWND window = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    std::map<std::string, HWND> controls;
    std::vector<Command> commands;
    native_ui::PanelBuffer background;
    RECT viewport{};
    unsigned applies = 0;

    ~Workspace() { if (font) DeleteObject(font); }
    int Px(float dip) const { return static_cast<int>(std::lround(dip * dpi / 96.0f)); }
    float Dip(int px) const { return px * 96.0f / dpi; }

    HWND Control(const char* id, const wchar_t* type, const wchar_t* label,
                 DWORD style, UINT command = 0, DWORD extended = 0) {
        HWND result = CreateWindowExW(extended, type, label, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(command)),
            GetModuleHandleW(nullptr), nullptr);
        if (!result) return nullptr;
        controls.emplace(id, result);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        return result;
    }
    // The sample registry is local C++; it is not the MMS plugin C ABI.
    bool RegisterCommand(UINT id, const wchar_t* label, std::function<void()> callback) {
        for (const auto& command : commands) if (command.id == id) return false;
        commands.push_back({id, label, std::move(callback)});
        return true;
    }
    void Status(const wchar_t* text) { SetWindowTextW(controls.at("status"), text); }
    bool Create() {
        HDC screen = GetDC(window);
        if (screen) { dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)); ReleaseDC(window, screen); }
        if (!dpi) dpi = 96;
        font = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) return false;
        if (!RegisterCommand(kApply, L"应用参数", [this] {
            wchar_t text[192]{}; GetWindowTextW(controls.at("name.input"), text, 192);
            std::wstring message = L"已应用：" + std::wstring(text);
            ++applies; Status(message.c_str()); InvalidateRect(window, &viewport, FALSE);
        })) return false;
        if (!RegisterCommand(kReset, L"恢复默认", [this] {
            SetWindowTextW(controls.at("name.input"), L"自适应工作区");
            SetWindowTextW(controls.at("value.input"), L"1.0");
            SendMessageW(controls.at("option"), BM_SETCHECK, BST_UNCHECKED, 0);
            applies = 0; Status(L"已恢复默认参数"); InvalidateRect(window, &viewport, FALSE);
        })) return false;
        if (!RegisterCommand(kRefresh, L"刷新预览", [this] {
            InvalidateRect(window, &viewport, FALSE); Status(L"仅重绘预览，不修改数据");
        })) return false;
        for (const auto& command : commands) {
            const auto id = "command." + std::to_string(command.id);
            if (!Control(id.c_str(), L"BUTTON", command.label.c_str(), WS_TABSTOP | BS_PUSHBUTTON, command.id)) return false;
        }
        return Control("heading", L"STATIC", L"参数分组", 0) &&
            Control("name.label", L"STATIC", L"名称（可输入中文）", 0) &&
            Control("name.input", L"EDIT", L"自适应工作区", WS_TABSTOP | ES_AUTOHSCROLL, 2001, WS_EX_CLIENTEDGE) &&
            Control("value.label", L"STATIC", L"示例数值", 0) &&
            Control("value.input", L"EDIT", L"1.0", WS_TABSTOP | ES_AUTOHSCROLL, 2002, WS_EX_CLIENTEDGE) &&
            Control("option", L"BUTTON", L"显示辅助标记", WS_TABSTOP | BS_AUTOCHECKBOX, 2003) &&
            Control("status", L"STATIC", L"拖动窗口边缘，观察按钮换行及参数分组重排。", SS_LEFTNOWORDWRAP);
    }
    native_ui::Node Leaf(const std::string& id, float width, float height) const {
        native_ui::Node item; item.id = id;
        item.measuredMinimum = {width, height}; item.measuredPreferred = {width, height};
        item.hints.grow.width = 1; item.hints.shrink.width = 1;
        return item;
    }
    native_ui::Node Field(const char* id, const char* label, const char* input) const {
        native_ui::Node field; field.id = id;
        field.hints.flow = native_ui::Flow::Column; field.hints.align = 3;
        for (auto& padding : field.hints.padding) padding = 0;
        field.hints.grow.width = 1; field.hints.shrink.width = 1;
        field.children = {Leaf(label, 170, 22), Leaf(input, 170, 30)};
        return field;
    }
    void Layout() {
        RECT client{}; GetClientRect(window, &client);
        native_ui::Node root; root.id = "workspace";
        root.hints.flow = native_ui::Flow::Column; root.hints.align = 3;
        root.hints.gap[1] = 10;
        for (auto& padding : root.hints.padding) padding = 12;
        native_ui::Node toolbar; toolbar.id = "toolbar";
        toolbar.hints.flow = native_ui::Flow::Wrap; toolbar.hints.gap[0] = 8; toolbar.hints.gap[1] = 8;
        for (auto& padding : toolbar.hints.padding) padding = 0;
        for (const auto& command : commands) toolbar.children.push_back(Leaf("command." + std::to_string(command.id), 124, 32));
        native_ui::Node fields; fields.id = "parameter.group";
        fields.hints.flow = native_ui::Flow::Grid; fields.hints.columns = 2;
        fields.hints.align = 3; fields.hints.gap[0] = 12;
        for (auto& padding : fields.hints.padding) padding = 0;
        fields.children = {Field("name.field", "name.label", "name.input"), Field("value.field", "value.label", "value.input")};
        root.children = {toolbar, Leaf("heading", 170, 24), fields, Leaf("option", 170, 28)};
        const float width = Dip(client.right), height = Dip(client.bottom);
        auto prefix = native_ui::Arrange(root, {width, 0});
        // Measure wrapping first; use the remaining height for the viewport.
        auto preview = Leaf("viewport", 170, std::max(100.0f, height - prefix.content.height - 54));
        preview.measuredPreferred.width = std::max(170.0f, width - 24);
        root.children.push_back(preview); root.children.push_back(Leaf("status", 170, 24));
        auto arranged = native_ui::Arrange(root, {width, 0});
        std::vector<native_ui::WindowMove> moves;
        RECT candidateViewport = viewport;
        for (const auto& item : arranged.items) {
            RECT rect{Px(item.rect.x), Px(item.rect.y), Px(item.rect.x + item.rect.width), Px(item.rect.y + item.rect.height)};
            if (item.id == "viewport") candidateViewport = rect;
            if (auto found = controls.find(item.id); found != controls.end()) moves.push_back({found->second, rect});
        }
        if (native_ui::ApplyWindows(moves) == native_ui::ApplyResult::Applied) viewport = candidateViewport;
        else Status(L"布局应用失败；库已尝试恢复原位置。");
        InvalidateRect(window, nullptr, FALSE);
    }
    void Paint() {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); if (!dc) return;
        RECT client{}; GetClientRect(window, &client);
        HDC memory = background.Begin(dc, client.right, client.bottom, dpi, RGB(240, 242, 245));
        HDC target = memory ? memory : dc;
        HBRUSH paper = CreateSolidBrush(RGB(240, 242, 245)); FillRect(target, &client, paper); DeleteObject(paper);
        HBRUSH ink = CreateSolidBrush(RGB(38, 45, 57)); FillRect(target, &viewport, ink); DeleteObject(ink);
        SetTextColor(target, RGB(235, 238, 242)); SetBkMode(target, TRANSPARENT);
        auto oldFont = SelectObject(target, font);
        RECT label = viewport;
        const wchar_t* text = L"布局占位区域（GDI）";
#ifdef TEMPLATE_VIEWPORT
        text = L"模拟视口（CPU / GDI，无场景渲染器）";
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(75, 85, 100)); auto oldPen = SelectObject(target, pen);
        for (int x = viewport.left; x < viewport.right; x += std::max(1, Px(24))) { MoveToEx(target, x, viewport.top, nullptr); LineTo(target, x, viewport.bottom); }
        for (int y = viewport.top; y < viewport.bottom; y += std::max(1, Px(24))) { MoveToEx(target, viewport.left, y, nullptr); LineTo(target, viewport.right, y); }
        SelectObject(target, oldPen); DeleteObject(pen);
#endif
        DrawTextW(target, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(target, oldFont);
        const bool retry = memory && !background.Copy(dc, paint.rcPaint);
        EndPaint(window, &paint);
        // Queue a new damage region after EndPaint has validated the old one.
        if (retry) InvalidateRect(window, &paint.rcPaint, FALSE);
    }
    bool GeometryValid() const {
        RECT client{}; if (!GetClientRect(window, &client)) return false;
        std::vector<RECT> rectangles;
        for (const auto& control : controls) {
            RECT rect{}; if (!GetWindowRect(control.second, &rect)) return false;
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
            if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom ||
                rect.right <= rect.left || rect.bottom <= rect.top) return false;
            for (const auto& previous : rectangles) { RECT overlap{}; if (IntersectRect(&overlap, &rect, &previous)) return false; }
            RECT overlap{}; if (IntersectRect(&overlap, &rect, &viewport)) return false;
            rectangles.push_back(rect);
        }
        return viewport.left >= 0 && viewport.top >= 0 && viewport.right <= client.right && viewport.bottom <= client.bottom;
    }
};
LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Workspace*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Workspace*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        host->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (!host) return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_CREATE: return host->Create() ? 0 : -1;
        case WM_SIZE: if (wp != SIZE_MINIMIZED) host->Layout(); return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED) for (const auto& command : host->commands) if (command.id == LOWORD(wp)) { command.invoke(); return 0; }
            return DefWindowProcW(window, message, wp, lp);
        case WM_ERASEBKGND: return 1; // WM_PAINT fills the complete parent buffer.
        case WM_PAINT: host->Paint(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {host->Px(320), host->Px(520)}; return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (...) {
        // No C++ exception may unwind through a system window callback.
        PostMessageW(window, WM_CLOSE, 0, 0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR arguments, int show) {
    SetProcessDPIAware(); // Vista+; this template intentionally uses system DPI.
    Workspace host;
    HDC screen = GetDC(nullptr);
    if (screen) { host.dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)); ReleaseDC(nullptr, screen); }
    if (!host.dpi) host.dpi = 96;
    WNDCLASSW type{}; type.hInstance = module; type.lpszClassName = L"MMS.Template.AdaptiveWorkspace";
    type.lpfnWndProc = Window; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&type)) return 1;
    RECT outer{0, 0, host.Px(760), host.Px(540)};
    AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, WS_EX_CONTROLPARENT);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = work.right > work.left ? std::min(outer.right - outer.left, work.right - work.left) : outer.right - outer.left;
    const int height = work.bottom > work.top ? std::min(outer.bottom - outer.top, work.bottom - work.top) : outer.bottom - outer.top;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"C++ 自适应 UI 模板",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, module, &host);
    if (!window) return 2;
    if (std::wcsstr(arguments, L"--self-test")) {
        bool valid = true;
        for (int testWidth : {900, 340, 900}) {
            RECT desired{0, 0, host.Px(static_cast<float>(testWidth)), host.Px(700)};
            AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, WS_EX_CONTROLPARENT);
            valid &= SetWindowPos(window, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
            valid &= host.GeometryValid();
        }
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(kApply, BN_CLICKED), 0); valid &= host.applies == 1;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(kRefresh, BN_CLICKED), 0); valid &= host.applies == 1;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(kReset, BN_CLICKED), 0); valid &= host.applies == 0;
        DestroyWindow(window); return valid ? 0 : 4;
    }
    ShowWindow(window, show); MSG message{}; int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return result < 0 ? 3 : static_cast<int>(message.wParam);
}
'''

CHECKS = r'''#include <native_ui/layout.hpp>
#include <native_ui/update.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
int main() {
    try {
        native_ui::Node group; group.id = "commands"; group.hints.flow = native_ui::Flow::Wrap;
        for (int i = 0; i != 3; ++i) {
            native_ui::Node button; button.id = std::to_string(i);
            button.measuredMinimum = {100, 28}; button.measuredPreferred = {100, 28}; group.children.push_back(button);
        }
        auto wide = native_ui::Arrange(group, {400, 0}); auto narrow = native_ui::Arrange(group, {150, 0});
        Check(wide.items.size() == 4 && narrow.items.size() == 4, "all commands stay registered");
        Check(narrow.content.height > wide.content.height, "narrow layout wraps to more rows");
        Check(group.children[0].measuredPreferred.width == 100, "layout must not rewrite preferences");
        native_ui::Node root; root.id = "root"; root.hints.flow = native_ui::Flow::Column; root.hints.align = 3;
        native_ui::Node tail; tail.id = "tail"; tail.measuredMinimum = {100, 20}; tail.measuredPreferred = {100, 20};
        root.children = {group, tail}; auto nested = native_ui::Arrange(root, {150, 0});
        float groupBottom = 0, tailTop = -1;
        for (const auto& item : nested.items) {
            if (item.id == "commands") groupBottom = item.rect.y + item.rect.height;
            if (item.id == "tail") tailTop = item.rect.y;
        }
        Check(tailTop >= groupBottom, "wrapped group must not overlap following content");
        native_ui::UpdateCoordinator queue;
        queue.Request({1, 1, 1, 0, native_ui::UiDirty}); queue.Request({1, 1, 1, 0, native_ui::LayoutDirty});
        unsigned calls = 0;
        queue.Drain(0, 8, [&](const native_ui::UpdateRequest& request) {
            ++calls; Check(request.flags == (native_ui::UiDirty | native_ui::LayoutDirty), "refresh requests coalesce");
            Check(queue.Drain(0, 8, [](const auto&) { return native_ui::UpdateOutcome{}; }) == 0, "no recursive callback");
            return native_ui::UpdateOutcome{native_ui::UpdateResult::Done, request.flags};
        });
        Check(calls == 1, "coalesced work executes once");
        std::cout << "PASS: wrapping, retained preferences, nested groups, coalescing, reentry guard\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''

README = r'''# C++ 原生自适应 UI 模板

本包提取 MMS 仓库的原创 `native_ui` 源码，并提供可独立编译的 Win32 示例。模板不依赖 MMS 主程序、插件运行时或图形设备。

## 编译示例

开发环境为 Windows、Visual Studio 2022 的「使用 C++ 的桌面开发」、MSVC v143、Windows SDK 10.0.19041.0 和 CMake 3.21 或更新版本。配置时选择 x64。示例按 C++17 编译，Release 静态链接 MSVC 运行库。

在解压后的根目录运行：

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A x64 "-DCMAKE_SYSTEM_VERSION=10.0.19041.0" "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release"
cmake --build out --config Release --parallel 4
ctest --test-dir out -C Release --output-on-failure
```

`layout_checks` 检查纯布局和请求合并。另三项 `--self-test` 创建隐藏窗口，不显示界面；其中工作区示例检查宽窄窗口下的控件边界、不重叠及命令回调。测试通过只证明这些检查成立，不代替输入法、连续拖动和多 DPI 的人工验收。

启动 `out/Release/adaptive_workspace.exe`。缩小窗口宽度后，命令按钮换行，参数区域从两列变为一列，预览区域跟随剩余空间变化。Tab 切换原生控件焦点，编辑框保留系统输入法行为。该示例使用系统 DPI，在启动时把 DIP 转换为像素；没有实现跨不同 DPI 屏幕时的动态字体与窗口缩放。

`native_form.exe` 和 `viewport_host.exe` 保留仓库已有独立示例。也可以分别把 `third_party/native_ui/examples/native_form` 或 `third_party/native_ui/examples/viewport_host` 作为 CMake 源目录构建；这两个原始示例沿用自身的编译选项。

## 修改模板

编辑 `examples/workspace.cpp`：

1. 在 `Workspace::Create` 中用 `RegisterCommand` 添加命令 ID、标签和回调。
2. 用 `Control` 创建原生控件。控件的字符串 ID 与布局叶节点 ID 保持一致。
3. 在 `Workspace::Layout` 中添加节点，声明最小尺寸、首选尺寸、分组与换行方式。
4. 在回调中更新业务数据，再请求需要的区域重绘。

`RegisterCommand` 是本示例的进程内 C++ 注册表，按钮通过 `WM_COMMAND` 调用同一回调。它不表示宿主已经开放 MMS 插件 ABI 中的全部 UI 服务。把模板改为 MMS 插件时，仍须查询实际服务与挂载能力，遵循宿主线程和生命周期规则。

`Workspace::Layout` 接收 `WM_SIZE`，先测量按钮换行和参数分组，再计算预览区域。它将 DIP 结果转换为父窗口客户区像素，统一交给 `ApplyWindows`。不要用屏幕坐标作为子窗口布局输入。

要接入实际渲染器，在预览位置创建自己的子窗口，使用同一布局结果更新它。当前深色区域只是 GDI 占位；原始 `viewport_host` 也只是模拟视口，不包含场景、物理、相机或 GPU 渲染服务。

## API 参考

完整声明位于 `third_party/native_ui/include/native_ui`。

| API | 当前能力 |
| --- | --- |
| `Wrap(items, width, gap, padding)` | 根据宽度换行；隐藏项不占位。 |
| `Node`、`Hints`、`Arrange(root, available)` | 行、列、换行、自动列网格、最小与首选尺寸、最大尺寸、伸缩、间距、对齐及隐藏占位。尺寸单位为 DIP，原始偏好不修改。 |
| `ApplyWindows(moves)` | UI 线程按父窗口批量移动原生子窗口，复核实际矩形；失败后尝试恢复。调用方须检查返回值。 |
| `PanelBuffer::Begin`、`Copy` | GDI 父窗口背景缓冲。调用方绘制损伤区域并检查复制结果；不保证每个原生控件内部无闪烁。 |
| `UpdateCoordinator` | 单 UI 线程请求合并、截止时间、代次检查、隐藏恢复与递归执行保护；不提供跨线程安全。 |

`UpdateCoordinator` 的脏标记只表达调用方要处理的工作。示例里的 `RenderDirty` 计数不代表库内有渲染器。持续动画、异步任务、主题、布局持久化、菜单溢出与自动快捷键注册没有在此模板中实现。

## 运行基线

根工程设置 `_WIN32_WINNT=0x0601`、`WINVER=0x0601` 和 PE 子系统 6.01。模板只用 CPU、Win32 和 GDI，不依赖 CUDA、NVAPI、AVX2、DirectComposition 或某个显卡厂商。系统 DPI 调用 `SetProcessDPIAware` 在 Win7 上可用，没有静态导入新系统 DPI 接口。

运行目标为 Win7 SP1 x64，并保留 CPU 和 AMD 兼容要求。设置目标宏、静态运行库或检查导入表不能替代目标系统验收。此包未完成 Win7 SP1 x64 和 AMD 真机验收，也没有把构建机上的检查写成这些硬件的通过结论。

## 来源与许可

源码快照、文件哈希与打包脚本 SHA256 见 `MANIFEST.json`。源码、新增工程、示例与说明均沿用随包 `LICENSE`，未重新授权。
'''

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/packages/mms-adaptive-ui-template"))
    parser.add_argument("--refresh", action="store_true", help="refresh only the exact known source-package file set")
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    destination = (repository / args.output).resolve()
    packages = (repository / "build/packages").resolve()
    if not destination.is_relative_to(packages) or destination == packages:
        parser.error("output must be a child of this checkout's build/packages")
    archive = destination.with_suffix(".zip")
    if (destination.exists() or archive.exists()) and not args.refresh:
        parser.error("output already exists; select another --output without deleting an existing package")
    source = repository / "third_party/native_ui"
    subprocess.run(["git", "diff", "--quiet", "HEAD", "--", "third_party/native_ui", "LICENSE"], cwd=repository, check=True)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip()
    originals = sorted(path for path in source.rglob("*") if path.is_file())
    allowed = {".hpp", ".cpp", ".md", ".txt", ".json"}
    if any(path.suffix not in allowed for path in originals):
        raise RuntimeError("unexpected file type in native_ui snapshot")
    files: dict[str, bytes] = {"LICENSE": (repository / "LICENSE").read_bytes()}
    for path in originals:
        files[path.relative_to(repository).as_posix()] = path.read_bytes()
    generated = {"CMakeLists.txt": CMAKE, "README.md": README,
                 "examples/workspace.cpp": WORKSPACE, "examples/app.manifest": MANIFEST,
                 "tests/layout_checks.cpp": CHECKS}
    for name, content in generated.items():
        files[name] = content.encode("utf-8")
    manifest = {"schemaVersion": 1, "name": "mms-adaptive-ui-template", "version": "0.1.0",
                "sourceCommit": head, "sourceDirectory": "third_party/native_ui",
                "packagerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "license": "LICENSE", "binariesIncluded": False,
                "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}}
    files["MANIFEST.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if args.refresh and destination.exists():
        existing = {path.relative_to(destination).as_posix() for path in destination.rglob("*") if path.is_file()}
        if existing != set(files):
            parser.error("existing directory differs from the exact known source-package file set")
    destination.mkdir(parents=True, exist_ok=args.refresh)
    for name, data in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for name, data in sorted(files.items()):
            entry = zipfile.ZipInfo(f"{destination.name}/{name}", date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            package.writestr(entry, data)
    print(json.dumps({"directory": str(destination), "archive": str(archive), "fileCount": len(files),
                      "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
