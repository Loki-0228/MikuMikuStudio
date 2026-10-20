# 原生菜单的 STATE 刷新

记录日期：2026-10-21。工作分支：`codex/plugin-abi`。

`MMS_UI_MENU` 和 `MMS_UI_MENU_ITEM` 已接入原 `mms.ui.refresh` 的 STATE 阶段。已挂载的原生菜单可以更新标签，并应用原命令的启用、选中和可见状态。full 和 original 两种配置的菜单专项均通过 1,088 项检查，完整 ABI 契约均通过 19,195 项检查。两种产品构建及 PE 基线检查通过，正式 release 与稳定 main 未修改。

## 接口行为

菜单继续注册到 `mms.menu.plugins` 及其注册后代。刷新目标仍是 `mms.ui.registerElement` 返回的原 handle，目标引用继续包含原 `instanceGeneration` 和 `projectEpoch`。`NativeUi` 保留真实父 `HMENU`、子 `HMENU`、原生菜单项 ID 和 `dwItemData` cookie。普通刷新按保留记录协调菜单，不清空并重建整棵子菜单。

菜单没有 HWND 客户区。目标查询返回以下能力和状态：

| 项目 | 实际行为 |
| --- | --- |
| `supportedKinds` | 仅 `MMS_UI_REFRESH_STATE`。 |
| `supportedScopes` | `SELF` 和 `SUBTREE`。`SUBTREE` 包含菜单本身及适用的注册后代，成员必须属于请求者。 |
| `width`、`height`、`geometryRevision` | 均为 0，不借用主窗口的客户区或几何修订。 |
| `ready` | 表示保留的原生菜单目标已发布且未暂停。未挂载的菜单目标返回 `MMS_E_NOT_READY`。 |
| `visible` | 菜单项本身的逻辑可见状态，不表示菜单当前是否展开，也不表示祖先当前可见。 |
| `REPAINT`、`DATA`、`LAYOUT` | 返回 `MMS_E_UNSUPPORTED`，包括 `STATE` 与这些阶段的组合。菜单不支持 DATA 绑定。 |
| 局部损伤矩形 | 不支持。仅 STATE 的请求携带非空 `dirtyRects`，按原公共参数检查返回 `MMS_E_ARGUMENT`。菜单请求的 `expectedGeometryRevision` 必须为 0。 |

菜单隐藏时，宿主从父菜单移除原生项，并保留子 `HMENU`。目标仍可接收恢复可见状态的 STATE 请求。恢复后继续使用原 ID、cookie 和子菜单身份。隐藏、禁用或原生项已失效的祖先会阻止其后代执行命令。

内容提供者继续通过 `MmsUiRefreshStateContentApiV1` 注册，并向原 builder 写入 64 字节 `MmsUiStateContentV1`。菜单只接受 `MMS_UI_STATE_LABEL`。未提供标签时使用元素的注册标签。菜单尚无说明及禁用原因的原生消费者，申请 `MMS_UI_STATE_DESCRIPTION` 或 `MMS_UI_STATE_DISABLED_REASON` 返回 `MMS_E_UNSUPPORTED`。

`MMS_UI_MENU_ITEM` 沿用已注册命令的 `getState`。`MMS_UI_MENU` 可以没有命令，此时启用状态默认为真，可见状态以注册状态为基线。菜单引用命令时，宿主读取原命令的 `enabled`、`checked` 和 `visible`，并将命令可见状态与注册可见状态相交。文本提供者不能改变这些权限，也不能使用 INPUT 的 80 字节标量扩展。

公开 C ABI 的布局、版本及服务名均未改变，无需新增 SDK 接口。现有 `StateContentProvider::Refresh` 默认不请求 LAYOUT，可以直接用于菜单。显式请求 LAYOUT 仍返回 `MMS_E_UNSUPPORTED`。

## 队列与生命周期

菜单请求使用原刷新队列、票据、配额、后台入队及重复贡献合并规则。菜单目标和有客户区的目标保存在同一目标表中，按呈现类型分别发布。普通窗口布局暂停不会暂停菜单目标，菜单发布失败也不会暂停工具栏或 INPUT。

主窗口的既有 lifetime subclass 处理真实 `WM_ENTERMENULOOP` 和 `WM_EXITMENULOOP`。菜单展开期间，宿主保留已接受的菜单请求，延后菜单 STATE 消费及结构移除。公开票据仍显示 `PENDING`，`cancel` 和 `release` 保持原语义。工具栏 STATE 及 INPUT DATA 可以继续执行。

只有被延后的菜单请求时，宿主不反复唤醒或启动重试定时器。退出菜单循环后，宿主通过原唤醒入口恢复待处理工作。新的工具栏请求不会被旧菜单票据阻止唤醒。关闭主窗口的请求先结束菜单循环，再通过原关闭消息完成，避免循环仍引用已销毁菜单。

元素替换、内容提供者替换、项目代次变化或所有者停止，会按原规则撤销旧票据及命令来源。原生菜单项因展开而暂时保留时，旧 ID 仍不能执行已失效来源。卸载后的结构清理由安全时点完成。

STATE 的准备及原生应用保持只读回调保护。提供者不能在回调中注册、注销、执行命令或递归提交同所有者的状态更新。菜单刷新不开放普通 `mms.edit`，不建立工程历史事务，也不读取属性值。源码路径只处理原生菜单状态，不调用场景求值、播放头切换或工程写入。

原生写入前，宿主准备标签、顺序和回滚数据。写入后读取真实文字、选中及启用状态，并重新验证来源。原生调用先修改后报错、文字读回不符或来源过期时，宿主只对仍为同一记录的菜单项补偿。失败目标保持暂停，后续普通菜单协调修复保留内容并重新发布，不将部分修改报告为成功。

## 四项审查修复

| 审查发现 | 实际修复 |
| --- | --- |
| 清理旧 `HMENU` 时仅调用 `IsMenu`。旧句柄失效后若被其他菜单使用，析构可能清空或销毁无关菜单。 | `MenuState` 析构及 `Detach` 在清空、销毁前检查 `MIM_MENUDATA` 的精确 cookie。cookie 已失效时只放弃旧句柄。测试将仍存活的菜单交给其他 cookie，确认其项目及句柄仍保留。 |
| 执行子菜单命令时，只验证子项及祖先菜单对象，未验证祖先菜单项仍存在或实际启用。 | `Command` 对每个祖先调用 `MenuMatches`，检查真实项目、ID、cookie、子菜单及状态。在祖先 `getState` 返回后再次验证，并在执行前复核子项。测试覆盖原生祖先移除、禁用、ID 改动及回调内移除。 |
| 最后一次来源检查之后仍调用原生菜单读回。读回重入改变 `projectEpoch` 后，代码仍可能发布候选缓存。 | 无变化路径在原生读回后重新检查来源。写入路径在最终读回后再次检查，确认成功后才发布缓存。测试分别在无变化及已写入路径的真实读回后改变项目代次。 |
| 新建菜单成功，但设置 cookie 失败。旧句柄的严格 cookie 清理规则不能回收尚未设置 cookie 的新分配。 | 新建路径在发布前单独持有新 `HMENU`。设置 cookie 失败时立即释放该次未发布分配，成功后才交给保留记录。测试拒绝根菜单及子菜单的首次 cookie 设置，并确认新句柄已销毁。 |

实现位置为 `src/plugins/native_ui.cpp` 的 `ApplyMenuState`、`Menu`、`RefreshMenus`、`Command` 和 lifetime subclass，`src/plugins/ui_registry.cpp` 的原状态及内容提供者入口，以及 `src/plugins/ui_refresh.cpp` 的原队列发布与消费路径。

## 真实通知夹具的修正

初次 `menu-full-01` 在第 282 项检查后失败，日志只记录进入、退出及检查回调的联合断言失败。当时不能据此判断原生循环是否启动。

`menu-full-02` 保留原断言并增加原生消息诊断。`TrackPopupMenuEx` 返回 1，错误码为 0。主窗口收到了 `WM_ENTERIDLE`，随后由 3 秒超时调用 `EndMenu` 结束循环，但进入、退出及初始化通知均为 0，检查回调没有投递或执行。

该夹具使用了 `TPM_NONOTIFY`。本机 USER32 的实测结果表明，该标志屏蔽了夹具依赖的通知。夹具改为普通通知及 `TPM_RETURNCMD`，继续通过真实 `EndMenu` 退出，没有合成菜单通知，也没有改变生产侧的菜单循环保护。

修正后的 `menu-full-03` 中，每次循环均收到一次真实进入及退出通知，检查消息投递并完成，未超时。后台请求保留、工具栏及 INPUT 可执行、菜单退出后唤醒和无空转断言均保留。早期两次运行仍按失败记录，不计为通过。

## 验收证据

| 检查项 | 当前结果 | 证据 |
| --- | --- | --- |
| full 菜单 STATE，`--menu-state-only` | 退出码 0，`PASS MENU STATE checks=1088`。 | `build/verification/abi-menu-selection-20261021/menu-full-03/report.json`。 |
| full 完整 ABI 契约 | 退出码 0，`PASS checks=19195`。 | `build/verification/abi-menu-selection-20261021/menu-contract-full-01/report.json`。 |
| original 菜单 STATE，`--menu-state-only` | 退出码 0，`PASS MENU STATE checks=1088`。 | `build/verification/abi-menu-selection-20261021/menu-original-01/report.json` 的专项条目。 |
| original 完整 ABI 契约 | 退出码 0，`PASS checks=19195`。 | `build/verification/abi-menu-selection-20261021/menu-original-01/report.json` 的契约条目。 |

full 和 original 产品及 ABI 测试程序已完成构建，记录位于同一证据目录的 `build-menu-full-05.log` 和 `build-menu-original-05.log`。上表的运行报告分别保留专项及完整契约的退出码和结果。

`pe-inventory-menu-final.json` 记录两份产品的 PE 信息。两份产物均为 x64，子系统版本为 6.1，`passBaseline=true`，`directOptionalImports` 为空。

| 配置 | 文件字节数 | SHA-256 |
| --- | ---: | --- |
| full | 9,403,904 | `fa78a7e7ee43762ae534798dea4c8b63445399672e81f516bdee2160bbd252ba` |
| original | 3,377,152 | `86b0d9930b198a9d495a4eae0dbf5d6cd3c5de990c23c2eb95a3330718cad78e` |

这些产物位于各自构建目录。正式 release 的 SHA-256 仍为 `180DB2580C99A3578F30B2539451FF82605AEBDEC337DAEBCE8B77EDF86CF65A`，本批未覆盖该目录。稳定 main 的最新提交仍为 `c147454`，本批未修改稳定 main。

计数及状态可从已有报告复核，无需重跑测试：

```powershell
Get-Content build/verification/abi-menu-selection-20261021/menu-full-03/report.json | ConvertFrom-Json | Select-Object profile, mode, inheritedDesktop, realPrivateDesktop, exitCode, last
Get-Content build/verification/abi-menu-selection-20261021/menu-contract-full-01/report.json | ConvertFrom-Json | Select-Object profile, mode, inheritedDesktop, realPrivateDesktop, exitCode, last
Get-Content build/verification/abi-menu-selection-20261021/menu-original-01/report.json | ConvertFrom-Json | Select-Object profile, mode, inheritedDesktop, realPrivateDesktop, exitCode, last
```

回归入口是 `tests/plugins/menu_state_refresh_checks.inc` 中的 `MenuStateRefreshChecks()`。夹具使用真实 `NativeUi`、原注册表、原 C 服务表、原队列及 USER32 菜单。MinHook 只链接测试程序，故障钩子仅在测试进程安装，并限定到夹具持有的菜单。钩子用于检查真实原生调用之后失败、文字读回异常、项目代次变化及新建资源失败，不进入产品实现或公开 ABI。

报告明确记录继承 `Default` 桌面，`realPrivateDesktop=false`。进程启动只请求 `SW_HIDE`，夹具可以显示自己的不激活窗口。测试没有发送全局鼠标或键盘输入，也没有独立桌面隔离。专项中的历史计数及属性读取检查来自插件运行时夹具，不是完整 MMS 场景或 GPU 渲染验收。Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保留，本批没有新增对应真机、IME 或多 DPI 人工验收。
