# MMDFacade 合入本地 abi

合并目标为本地 `abi` 的 `25900eca147ddb757695327b8bbb741578eeaf62`，
来源为 `feature/mmdfacade-compat` 的 `123feeee1e491fa52b29ed024ccf11810aa56f6d`。
采用保留双方历史的合并，不推送远端；`abi` 工作区其他任务的未提交改动单独保留。

## 冲突处理

唯一提交内容冲突位于 `src/render/model_renderers.cpp`。
保留 `abi` 的 `submit()` 调用和 PMX 点线模式、剔除状态恢复、顶点色处理，
将 full/original 两种 `submit()` 内的实际绘制转接到 MMDFacade。
保留 `abi` 已有的 MME 重复绘制优化和 DX12 视口服务。
MMDFacade 的 DLL 命名及 `PLUGINS/LEGACY/MMDPlugin.dll` 发布布局保持不变。

## 合并结果验证

为避开 `abi` 正在进行的其他工作，在 `D:/MMD-Next/MMS-ABI-MMDFacade` 的独立目录构建和测试。
Windows x64 Release / MSVC 19.44 / full / DX12 编译开关开启，使用本地 modern Conan 工具链。
复用 `abi` 已有资源文件；合并未改变资源输入，摘要记录在 `build/merge-resource-reuse.json`。

| 验证 | 结果 |
| --- | --- |
| Release 构建 | 主程序、真实插件测试宿主、相机和回调测试宿主编译链接通过；`build/merge-build.log` |
| CTest | 4 项通过，包括路径预检、9 组相机输入、72 组回调和 PowerShell 打包检查；`build/merge-ctest.log` |
| 真实 Effekseer + MME 0.37 | 新 LEGACY 路径下导入、渲染、重置、卸载、重启和模型删除通过；启用/重置/重启相对停止各改变 998 像素；`build/mmdfacade-functional-_vohnz6h/summary.json` |
| 真实 Effekseer，无 MME | 同上，并验证失败 Reset 后恢复；`build/mmdfacade-functional-mp_c_ztd/summary.json` |
| FirstPersonCamera 适配插件 | 上述两组真实窗口的键盘与鼠标输入通过；原版 MMDMainData 插件仍不在支持范围内 |
| 实际主程序 | 三种 EXE 名（含中文）× 三处提供者位置，共 9 组 MME 加载、MMDUtility 菜单与正常退出检查通过；`build/mmdfacade-product-awyw7qf3/summary.json` |

真实插件功能测试使用 D3D9 渲染路径；DX12 编译通过不等于旧插件在 DX12 下可用。
本次未重跑 Win7 真机、完整 full/minimal ZIP 发布及复杂特效组合。
既有 ABI 支持边界及其他未验证项继续遵循 [MMDFacade 说明](../MMDFACADE.md)。
