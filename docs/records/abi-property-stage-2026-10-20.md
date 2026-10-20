# 属性暂存与准确事务授权

`mms.ui.stageProperty` 已在受控编辑宿主中实现，使用原 C ABI、插件上下文、属性记录和共同历史。普通启动仍不提供 `mms.edit`，属性暂存返回 `MMS_E_NOT_READY`。声明式 INPUT、IME 草稿、原生输入生命周期和专用便利封装仍未实现。调用契约见 [SDK 属性暂存参考](../../sdk/mms/property-stage.md)。

## 实现范围

可写属性要求 UNDOABLE 和 stage 回调，严格校验类型、分量、数值范围、选项及默认值。值在进入插件前深复制。stage 必须来自原插件已授权的顶层命令或 UI 任务；进入事务前不修改命令回调深度。

`EditStore::StageProperty` 持有属性注册 guard，`Manager` 和 `UiRegistry` 共同限定原插件、准确事务和当前回调帧。只有该范围内的 join 可以加入候选；普通编辑、其他事务及嵌套 read/state 不继承权限。后续 stage 可更新同一 participant 的 after，保留第一次的 before，提交一次产生一个历史节点。

接受后的失败取消整个准确事务，并释放此前加入的全部参与者；同一 owner 的另一个事务保留。回调授权在取消前失效。候选、属性 guard 和最后一个回调捕获对象的销毁均处于 Busy 内，成功获得的宿主锁保留到清理完成。取消违约继续清理其他参与者并冻结历史。

准备和提交复核工程修订、属性注册身份及序列化器恢复令牌。相同 handle 的属性更新产生新记录，旧候选因此过期；无关 UI 更新不影响候选。guard 不进入已提交历史。五类 participant 回调通过无分配的只读 UI runner，不能在后台 UI 任务中绕过注册保护。[序列化器保护](abi-edit-provider-guard-2026-10-20.md)也覆盖 stage。

本批同时修复两类错误后的租约释放：属性 read 和 participant getUndoState 在填租约后抛异常，宿主仍释放恰好一次；提供者释放期间保留只读保护。void commit 抛异常会冻结历史并返回 FAILED，不再次取消已消费的 context。

## 验收结果

- 完整与原始兼容配置的属性专项各通过 5055 项，覆盖值校验、深复制、回调隔离、两事务精确取消、注册与序列化器失效、异常、容量、最后析构及真实 DLL 的撤销重做。
- 临时私有桌面中，两配置完整 `plugin_abi_contract` 各通过 9345 项；曲线编辑器原断言通过 2538 项。
- 8 组原生历史共 15539 项通过，包括轮廓数据与真实控件、模型顺序、物理、音频、历史模式、附件设置及模型 VMD。
- 两种产品配置编译成功。完整产品原名及中文改名的隐藏启动、菜单、工具栏、面板实时侧栏布局和退出通过。原始兼容 profile 仍不在产品内加载 ABI 插件。

共享桌面的第一次广回归中，曲线编辑器 Down 键光标断言失败；原配置完整契约在溢出菜单三秒关闭期限处失败，未进入属性专项。同一测试程序在私有桌面通过，未修改或放宽断言。记录保留，不将单次通过写成所有桌面状态下均无问题。

产品探针首次误用了会切换隐藏布局的 `gui-layout-live` 模式，触发了普通可见工具栏检查；改用与该脚本验收路径一致的 `gui-panel` 后，原名及改名均通过。没有为此更改产品逻辑或测试断言。

证据目录为 `build/verification/abi-property-stage-20261020/`。重点日志为 `test-focused.log`、`test-focused-original.log`、`private-desktop/report.json`、`native-history/report.json`、`product-panel.log`。广回归失败分别保存在 `test-final.log`、`test-original-final.log`。本批没有 Win7 或 AMD 真机验收，不增加基础系统或显卡依赖。main 源码、稳定版程序及外部未提交修改未改动。

ABI 发布目录的 `build/release/MikuMikuDance.exe` 已更新为同一受验程序，大小 9015296 字节，SHA-256 为 `1C457DAF675E2D9F417D0714C9E65D34600FAC101458F51B0A11A84E87F940D0`。上一版保存在证据目录的 `previous-abi-MikuMikuDance.exe`；测试插件未加入发布目录。

复验属性专项：

```powershell
& D:/CMake/bin/cmake.exe --build build/cmake --config Release --target plugin_abi_tests --parallel 4
& build/cmake/tests/Release/plugin_abi_tests.exe --property-stage-only build/cmake/tests/plugins/edit/Release/manager_edit_probe.dll
```
