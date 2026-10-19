# 插件自行注册菜单

MMS 导出 `MMSGetMenuHostApi`，版本为 `MMS_MENU_ABI_VERSION == 1`。公开 C 头文件为 `include/mikumikustudio/plugin_menu.h`。插件通过 `GetProcAddress(GetModuleHandleW(nullptr), "MMSGetMenuHostApi")` 查询，不需要链接 MMS 导入库，也不需要把插件源码加入宿主构建。

宿主不预建 MMV、MME 或 MMHack 的菜单。每个成功注册的插件获得一个右对齐的顶层入口和 64 个独立命令 ID；标题、菜单内容、勾选状态、命令及快捷键均来自插件回调。没有 DLL、DLL 无效、初始化失败或插件未注册时，不出现对应入口。

## 接入流程

1. 在拥有主窗口的 UI 线程、`DllMain` 之外查询版本 1 接口。结构体的 `structureSize` 必须初始化。
2. 填写 `MmsPluginMenu`：主窗口 `owner`、稳定且唯一的 `pluginId`、插件上下文，以及 `title`、`populate`、`command` 回调；`shortcut` 可选。
3. 调用 `registerMenu`，保存非零 token。重复的窗口和插件 ID 会被拒绝。
4. `populate` 只操作传入的 popup。使用 `firstCommand + [0,63]` 作为命令 ID，收到的 `commandOffset` 为相应偏移。宿主保留 `0xD000` 至 `0xEFFF`，不复用已退役 ID，避免排队命令误投给新插件。
5. 状态改变后可调用 `refreshMenu(token)`；语言切换、菜单资源替换和菜单打开时也会刷新。语言 0/1/2 分别为日语、英语、中文。
6. 销毁上下文或卸载 DLL 前调用 `unregisterMenu(token)`。窗口销毁会移除其全部注册。宿主保留回调模块引用直到最后一个正在分发的回调退出。

回调不能抛出异常。只有命令和快捷键回调可以增删注册；标题和填充回调不能修改注册表。菜单句柄属于宿主，插件不能销毁它或把其他菜单的子树挂入它。

```cpp
auto get = reinterpret_cast<MmsGetMenuHostApiFn>(
    GetProcAddress(GetModuleHandleW(nullptr), "MMSGetMenuHostApi"));
MmsMenuHostApi host{sizeof(host)};
if (get && get(MMS_MENU_ABI_VERSION, &host)) {
    MmsPluginMenu menu{sizeof(menu), MMS_MENU_ABI_VERSION};
    menu.owner = ownerWindow;
    menu.pluginId = "MyPlugin";
    menu.context = context;
    menu.title = Title;
    menu.populate = Populate;
    menu.command = Command;
    token = host.registerMenu(&menu);
}
```

旧版 MME/MMHack 自行通过 Win32 插入菜单的行为保持兼容；本次没有修改这些第三方 DLL。MMV4MMS 已采用此接口，菜单实现位于其独立仓库的 `src/mms/runtime_menu.inl`，ABI 快照位于 `include/mms/plugin_menu.h`。

## 验证

Release x64 隐藏窗口检查覆盖：缺失和损坏 DLL 不显示入口、公开导出和版本检查、三个独立注册者的命令隔离、旧 MME 菜单共存、三语菜单、快捷键、菜单资源替换、重复注册、回调内注销、过期命令和窗口销毁。

无 DLL 的同条件基线显示 2 个顶层菜单（原有 File 加错误的 MMV），修复后仅显示原有 File。对应测试 EXE SHA-256 分别为 `28f73f54245138669aad524df8a74b0a78d4005687c528f9f339f82aa62a81e1`、`bc79746381981474aff0c49b26939fe50997036fbc7cd8ed3e3c95921654b43f`。源码基线为 `d8d4277`，使用相同 Release x64 配置、隐藏 640×480 窗口和相同断言。

扩展后的真实 DLL 回归检查了三语标签、启用及自动重载开关、`Ctrl+Shift+E`、菜单替换和卸载。日志和运行目录保留于 `.local/artifacts/plugin-owned-menus/`。未测 FPS 或 GPU 时间；菜单验证不代表 FX 画质、性能或所有第三方 DLL 已适配。
