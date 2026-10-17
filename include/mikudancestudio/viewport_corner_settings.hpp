#pragma once
// ===========================================================================
// 操作柄停靠角的程序目录设置（跨工程通用）
//
// 存储：data/settings.json 的 viewport 节
// 内容：corner 字符串为 tl / tr / bl / br
// 默认：br（右下）。缺文件 / 缺键 / 非法值一律回退默认。
//
// 这是当前程序目录的界面偏好，绝不写进 PMM / VMM / VEM：本模块不 include
// 任何工程文件读写，pmm_save / pmm_load / vmd / mmv storage 一行不动。
// 旧 data/viewport.ini 仅在中央 viewport 节缺失时导入。
// ===========================================================================
#include <string>

#include "mikudancestudio/viewport_tool_layout.hpp"

namespace mikudancestudio {
namespace viewport_corner_settings {

using Corner = viewport_tool_layout::Corner;

// 内存中的当前值。首次调用时读取中央设置；读不到即默认右下。
Corner Current();

// 更新并写盘（写失败只留内存值，不弹框）。调用方负责随后刷新视口。
bool Set(Corner corner);

// 中央 settings.json 的绝对路径，无法获取程序目录时返回空串。
std::wstring SettingsPath();

// 显式旧 INI 工具接口，供迁移及独立文件往返测试使用。
bool LoadFrom(const std::wstring& path, Corner* out);
bool SaveTo(const std::wstring& path, Corner corner);

// 仅测试用：直接设定内存缓存，绕开真实设置路径。
void ResetCacheForTest(Corner corner);

}  // namespace viewport_corner_settings
}  // namespace mikudancestudio
