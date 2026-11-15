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

## 最终插件目录修正

按用户澄清，删除发布包根目录的 `optional-plugins`；第一人称完整插件包直接放到
`PLUGINS/first-person-camera`。`PLUGINS/LEGACY` 可归放旧 MMD 插件包，仍由同一
`PLUGINS` 递归扫描发现；不新增独立扫描入口。构建、CMake 安装及两个打包入口均同步。

实际发布包启动检查发现原相机清单使用了宿主保留的 `mms.*` ID，已修正为
`org.mikumikustudio.legacy.first-person-camera`，Effekseer 安装示例同样避开保留前缀。
相机输入矩阵改为直接使用构建生成的正式清单，防止测试清单与实际发布包脱节。

14 项打包测试、9 组正式清单相机输入及 PowerShell 打包检查通过；实际发布包和安装树
均不再包含 `optional-plugins`，DLL 摘要与清单匹配。中文 EXE 的实际发布包探针验证
相机启用/禁用、`PLUGINS/LEGACY` 内旧窗口插件发现，以及故意放在根目录
`optional-plugins` 内的完整插件包被忽略。证据为 `build/camera-layout-*.log`、
`build/camera-release-5xakpm9i/summary.json`。探针目录与正式发布目录相互独立。

## 本地 abi 最终落地

合并准备期间，其他任务已将原先 19 个未提交文件提交为 `ebc79ca`。
最终合并保留该提交，四处同文件修改自动合并成功；没有删除这些修改或把它们重写为本次提交。
上面的隔离验证发生在这次同步之前；最终合并提交后再执行 x64 Release 构建和插件回归，
对应本地证据使用 `build/final-abi-*.log` 前缀。不推送远端。
