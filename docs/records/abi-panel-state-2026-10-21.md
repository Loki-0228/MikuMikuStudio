# 已挂载插件面板的 STATE 刷新

记录日期：2026-10-21。工作分支：`codex/plugin-abi`。

`MMS_UI_PANEL` 已接入原 `mms.ui.refresh` 的 STATE 阶段。已创建原生 surface 的面板可以更新页签标签、说明和禁用原因，并应用已有命令的启用及可见状态。本批通过 full 和 original 两种配置的自动验收及产品构建；原正式 release 目录未更新。

## 实现范围

面板仍注册到 `mms.workspace.tabs.extensions`，由原 surface 工厂创建 HWND。`NativeUi::Presentations` 仅为已有真实 surface 的面板发布 STATE 能力。`UiRegistry::ExistingSurface` 返回原 HWND、呈现实例、可见状态、边界和 DPI，刷新不创建替代窗口。

内容提供者继续通过 `MmsUiRefreshStateContentApiV1` 注册，`prepare` 向原 builder 写入 64 字节 `MmsUiStateContentV1`。面板只接受以下文本字段：

| 字段 | 原生显示位置 |
| --- | --- |
| `MMS_UI_STATE_LABEL` | 面板页签标签。未提供时使用注册标签。 |
| `MMS_UI_STATE_DESCRIPTION` | 页签区域的说明提示。未提供时沿用已有命令说明。 |
| `MMS_UI_STATE_DISABLED_REASON` | 禁用时与说明合并显示在页签提示中。 |

这些文本不修改插件自有 surface 内的控件文字。插件视图中的编辑草稿、控件实例和窗口内容由原插件持有，STATE 不重建该视图。

面板没有 `commandId` 时，启用状态默认为真，可见状态以注册状态为基线。面板引用已有命令时，宿主调用原 `getState`，应用 `enabled`，并将命令的 `visible` 与注册可见状态相交。`checked` 不表示当前活动页签，也不写入 surface HWND。面板分支不发送 `BM_GETCHECK` 或 `BM_SETCHECK`。

80 字节 `MmsUiInputStateContentV1` 的 `enabled`、`readOnly`、`visible` 扩展仍只用于 INPUT。面板内容提供者不能用这些字段改变权限。提供者申请 INPUT 专用字段时返回 `MMS_E_UNSUPPORTED`。

启用状态与页签选择分别保留。禁用或隐藏面板前，宿主安全移走面板及其后代控件的焦点；恢复启用不会重建 surface。面板实际显示还取决于当前活动页签及工作区可用空间。

可见状态变化需要 `STATE | LAYOUT`，以便重新排列页签和工作区。只有 STATE 的请求遇到这种变化时返回 `MMS_E_UNSUPPORTED`，不提前隐藏窗口。后台请求、SELF、SUBTREE 和重复请求合并继续由原队列处理。

## 原身份、权限和票据

刷新目标继续使用 `mms.ui.registerElement` 返回的原 handle，以及原 `MmsUiTargetRefV1` 中的 `instanceGeneration` 和 `projectEpoch`。面板状态缓存以该注册身份、真实 HWND 和 surface 实例为依据，不增加第二套对象注册表或公开 C ABI。

页签切换沿用原 `TCN_SELCHANGE` 处理和 surface 生命周期回调。允许获得焦点的面板仍通过原 `focusView` 接收焦点。标签更新、切换及布局不会替换 surface，也不会产生另一套事件身份。

请求仍返回原队列票据。提供者替换使旧 STATE 请求失效，但不因此取消独立的 REPAINT 请求。元素被替换、所有者停止、呈现实例改变或项目代次改变时，宿主拒绝旧来源；卸载继续按原队列及注册表清理规则取消请求并销毁视图。

内容准备和应用沿用原只读回调及刷新因果范围。提供者不能在回调中注销元素，也不能递归提交同所有者的 STATE 请求。本批不开放普通 `mms.edit` 入口，UI 刷新不建立工程历史事务、不读取属性值，也不写入工程数据。

主要实现位于 `src/plugins/ui_registry.cpp` 的 `WithElementState`、`WithElementStateContent`、`ExistingSurface` 和 `ShowSurface`，以及 `src/plugins/native_ui.cpp` 的 `ApplyPanelState`、`SyncPanelTabs`、`SyncPanelTips` 和 `LayoutPanels`。`src/plugins/ui_refresh.cpp` 按原目标类型分流面板，保留原请求顺序、结果及失效检查。

## 本批修复的故障

| 故障 | 修复及恢复规则 |
| --- | --- |
| `showView` 先隐藏 HWND 再抛异常，缓存仍为可见，补偿因缓存相等而跳过。`resizeView` 先移动 HWND 再抛异常也存在同类差异。 | `ShowSurface` 同时检查实际 `WS_VISIBLE` 和父窗口坐标中的真实边界。缓存相等但原生状态不符时仍调用原回调修复，并读回结果。面板 STATE 只在精确来源仍有效的补偿期间延后本次失败造成的暂停，所有失败退出最终暂停几何和刷新目标。补偿再次失败后，普通 Layout 可以重试修复同一 surface 并重新发布。 |
| `TCM_SETITEMW` 已写入候选标签，但重入使 `projectEpoch` 变化，票据返回 STALE。缓存保留旧标签，后续刷新因原生标签不符而无法恢复。 | 旧票据失效后不继续向新来源写入。下一次普通 Layout 以当前保留内容及精确注册、HWND、实例快照修复原页签文字和提示，不重建视图。新项目代次的提供者需要取得新 token。 |
| 原生显示及面板缓存已发布后，再向 `retainedVisibility` 插入节点。首次插入分配失败会使票据失败，但显示与缓存已改变。 | 删除面板不消费的该镜像写入。面板可见状态由原 surface 实例的面板缓存持有。页签顺序、活动标签及文字验证缓冲在原生消息前准备；最终发布使用已准备的存储。 |

原生标签拒绝写入、截断候选文字、提供者异常、分配异常及非法 UTF-8 均保留原有效内容或使目标暂停，不能把部分原生修改报告为成功。后续修复仍检查当前来源，不用旧票据修改替代视图。

## 验收证据

两份报告均记录退出码为 0：

| 检查项 | full | original |
| --- | ---: | ---: |
| PANEL STATE，`--panel-state-only` | 530 | 530 |
| INPUT STATE，`--input-state-only` | 1559 | 1559 |
| 属性挂载，`--property-mount-only` | 681 | 681 |
| 完整 ABI 契约 | 18095 | 18095 |

报告文件为：

- `build/verification/abi-panel-background-lifetime-20261021/panel-full-01/report.json`。
- `build/verification/abi-panel-background-lifetime-20261021/panel-original-01/report.json`。

产品构建日志 `build-full-02.log` 和 `build-original-01.log` 位于同一证据目录，均生成各配置的 `MikuMikuDance.exe`。追加的 full ABI 测试构建记录为 `build-panel-full-03.log`，生成 `plugin_abi_tests.exe`。这些构建产物位于各自构建目录，不表示正式 release 已更新。

计数可以直接从报告的 `last` 字段复核，无需重新执行测试：

```powershell
Get-Content build/verification/abi-panel-background-lifetime-20261021/panel-full-01/report.json | ConvertFrom-Json | Select-Object profile, mode, exitCode, last
Get-Content build/verification/abi-panel-background-lifetime-20261021/panel-original-01/report.json | ConvertFrom-Json | Select-Object profile, mode, exitCode, last
```

`tests/plugins/panel_state_refresh_checks.inc` 使用真实 `UiRegistry`、原 provider 与 refresh C 表、`NativeUi`、页签及 surface HWND。覆盖两面板原身份、插件编辑文本保留、无命令基线与提供者、默认回退、启用及显隐、后代焦点、后台入队、SUBTREE 合并、来源替换、卸载，以及上表三类故障和普通 Layout 恢复。测试入口为 `PanelStateRefreshChecks()`。

本轮测试继承 `Default` 桌面，报告明确记录 `realPrivateDesktop=false`。启动参数只请求 `SW_HIDE`；夹具可以显示自己的不激活窗口。测试不发送全局输入，不能据此声称真实桌面隔离、真实 IME 或多 DPI 人工验收通过。

## 保留边界

尚未实例化的页签没有真实 surface HWND，`getTarget` 继续返回 `MMS_E_NOT_READY`。宿主不会借用共享 tabs HWND 伪造面板目标。首次选择并创建 surface 后，才能查询和使用该面板的 STATE 能力。

本 PANEL 批次没有增加菜单、DOCK 或其他挂载根的刷新能力，也不扩大编辑权限。MENU/MENU_ITEM 的无几何 STATE 由后续[菜单批次](abi-menu-state-2026-10-21.md)单独实现及验证。Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保留，本轮没有新增对应真机或 GPU 渲染验收。
