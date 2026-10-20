# 共享曲线写入与历史回调的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

本批将 `Source.apply` 和 `Session.history.traverse` 接入已有的 `EditorLease`、共享 `Session` 版本及 `InvokeProvider`。回调在复制、调用或释放闭包时关闭窗口、同窗口替换会话、关闭后重建窗口，旧调用均停止。新会话不接收旧调用的通道、历史或界面状态。

实现位于 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，测试位于 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)。本批不增加公开 C ABI 或宿主服务。此前的[来源读取、查询和标签保护](abi-curve-source-lifetime-2026-10-21.md)及 [motion 文件选择保护](abi-curve-modal-lifetime-2026-10-20.md)继续保留。

## 回调与参数的生命周期

`InvokeProvider` 在复制闭包前固定原 `Session` 存储、会话身份和来源索引。复制后先复核身份，再调用副本。调用结束后释放副本及会话存储引用，随后再次复核。复制构造、回调执行或最终释放中发生的会话替换，都不能继续执行旧会话的后续操作。

普通提交的 `apply` 参数来自本次调用私有持有的通道。撤销和重做的参数来自移出历史栈的独立节点。窗口关闭或来源替换不会销毁正在传给回调的通道、名称、关键帧和错误输出。`traverse` 的方向按值传递，错误输出同样属于当前调用。

`RetiredContinuation` 使旧调用退出，现有 Win32 及公开入口在各自边界结束该调用。已完成的宿主写入保留，新会话不接收旧调用的刷新或恢复。持有旧身份 token 的强引用不使旧会话继续有效。

## 本地历史节点的稳定所有权

本地撤销和重做栈改为 `vector<unique_ptr<Snapshot>>`。节点中的通道和选择集合保持独立存储，栈扩容只移动 `unique_ptr`，不会复制 `Channel.sample` 闭包。

`CommitLocalChannels` 和 `TraverseLocalHistory` 在调用 provider 前准备节点、复制选择集合，并预留目标栈容量。撤销或重做只移出源栈的节点所有权，暂时保留一个空槽。实时通道通过 `swap` 转入私有节点。

回调拒绝、抛异常或复制闭包失败时，`LocalWriteRollback` 通过通道及选择集合的 `swap` 恢复状态，并将节点所有权放回源空槽。恢复不分配内存。恢复前核对原会话及来源身份，旧节点不会写入替换会话。

原会话仍有效且回调返回 `true` 后，历史通过已预留的栈容量转交 `unique_ptr`，选择集合通过 `swap` 转交。这里不构造新的 `Snapshot`。清除重做分支及裁掉最老撤销节点时，先把待释放节点移到独立局部容器。释放结束后再复核身份，避免闭包析构重入正在修改的实时历史容器。

无分配保证限于节点转交和失败恢复。provider、本次提交前的准备、规范值读回和界面刷新仍可分配。

## MSVC 集合移动的修正

早期完整配置构建在 `Snapshot` 的无异常移动构造断言失败，见 [build-floor-full-05.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-full-05.log)。失败位置是 `is_nothrow_move_constructible_v<Snapshot>`。

`Snapshot` 含有 `std::set`。MSVC 的集合移动构造可能为哨兵分配内存，因此按值移动 `Snapshot` 不能承担提交后的无分配保证。`vector<Snapshot>::reserve` 还可能退回复制已有节点，触发 `Channel.sample` 的用户闭包复制。

修复没有删除断言后继续使用原设计。当前代码校验 `unique_ptr<Snapshot>` 的移动及成员 `swap` 的无异常属性，并把节点和集合哨兵的分配放在 provider 调用之前。回归还在宿主接受写入后，注入下一次实际 C++ 分配的 `bad_alloc`，检查已接受数据和历史节点均未丢失。

## 接受写入后的读回失败

`AdoptWrittenChannels` 先安装已接受的私有通道，再读取宿主规范值。规范值读取失败或分配失败时，编辑器保留这些通道作为显示后备，并暂停继续编辑。恢复读取只更新界面及可编辑状态，不再调用 `apply`，也不再次转移历史节点。

外部共同历史的提交和 `traverse` 成功后，沿用 `externalReadPending` 读回流程。查询或读取失败可以重试，已经成功的提交及历史遍历不重试。

上述接受条件是回调返回 `true`。provider 返回 `false` 或抛异常时，仍须遵守原接口约定，保持宿主数据和共同历史不变。

## 同来源的递归编辑拦截

本地来源的提交前读取也设置 `externalCallbackActive`。本地及外部写入的 `externalOperationActive` 持续覆盖历史节点准备、写回闭包复制、调用、释放和成功后的规范值读回。

这些保护期间，父窗口命令、命令尾部的选择发布、画布拖拽预览及提交、双击插帧、MMD 操纵器和数值提交、通道树的编辑输入均停止写入。来源变更通知保留为待处理请求，不递归读取临时移空的状态。写入结束后，通知读取已完成的结果。

拥有当前写入的内部规范值刷新和选择发布可继续执行。随后已另行完成[选择回调生命周期迁移](abi-curve-selection-lifetime-2026-10-21.md)，本地撤销和重做改由实际命令尾部各发布一次选择，避免重复通知。

## 验证

主任务统一构建并运行测试。本记录只读取源码、日志及最终报告，没有另行构建或运行。

两套最终报告均已结束，所有条目的 `exitCode` 为 `0`、`error` 为 `null`：

- [完整配置报告](../../build/verification/abi-panel-background-lifetime-20261021/curve-write-full-01/report.json)。
- [原始兼容配置报告](../../build/verification/abi-panel-background-lifetime-20261021/curve-write-original-01/report.json)。

| 测试范围 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| `--write-lifetime` | 3,259 项通过。 | 3,259 项通过。 |
| `--source-lifetime` | 2,463 项通过。 | 2,463 项通过。 |
| `--motion-lifetime` | 278 项通过。 | 278 项通过。 |
| 编辑器全集 | 8,222 项通过。 | 8,222 项通过。 |

新增写入专项及全集包含以下回归：

- 本地提交、撤销、重做和外部共同历史的提交、撤销、重做，在闭包复制、调用及副本析构时关闭、替换或重建窗口。覆盖成功、拒绝及异常。
- 私有通道和错误输出在旧会话失效后仍可读取。旧调用不继续读回或查询，新会话保持可编辑。
- 已接受写入后的规范值读取中关闭或替换窗口，宿主操作仍只提交一次。
- 本地提交、撤销和重做遇到闭包复制 `bad_alloc`、普通拒绝及回调异常时，两条历史栈和宿主数据保持原状。重试只转移本次节点。
- 规范值读取失败及宿主接受后的一次实际分配失败，保留已接受数据和历史。恢复读取不会重复提交，后续反向遍历仍可用。
- 提交前读取、闭包复制、调用、释放和规范值读回期间，通过真实 `WM_COMMAND`、来源通知、画布及 MMD 消息触发嵌套编辑，宿主写入和选择发布不会递归执行。

修正后的完整产品及曲线测试构建通过，日志为 [build-floor-full-09.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-full-09.log)和 [build-curve-full-10.log](../../build/verification/abi-panel-background-lifetime-20261021/build-curve-full-10.log)。原始兼容产品及曲线测试构建也通过，日志为 [build-floor-original-02.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-original-02.log)和 [build-curve-original-03.log](../../build/verification/abi-panel-background-lifetime-20261021/build-curve-original-03.log)。主任务记录四次构建退出码均为 `0`。

运行继承桌面 `Default`，报告中的 `realPrivateDesktop=false`。`SW_HIDE` 只是进程启动请求，窗口显示由夹具控制。本批不能认定为独立桌面隔离、真实 IME、多 DPI 或目标硬件验收，详见[本机窗口回归更正](abi-verification-desktops-2026-10-21.md)。

## 剩余范围

最终两套产品的 PE 清单见 `build/verification/abi-panel-background-lifetime-20261021/pe-inventory-final.json`：均为 x64、子系统 6.1，未新增可选图形后端或厂商 DLL 的直接导入。清单只核对所列 PE 项，不替代逐 API、运行库部署或 Win7 真机验收。正式 `build/release/MikuMikuDance.exe` 的 SHA-256 仍为 `180DB2580C99A3578F30B2539451FF82605AEBDEC337DAEBCE8B77EDF86CF65A`，没有被本批构建覆盖。

比较模式与相邻控制开关已由[后续批次](abi-curve-comparison-lifetime-2026-10-21.md)统一到现有保护。以下回调仍未统一到 `InvokeProvider` 的复制、调用和释放检查：

- `Session.currentFrame`、`seek`、`isPlaying`、`setPlaying`。
- `Source.subdivide`、`Channel.sample`。

稳定历史节点避免了栈扩容时复制 `Channel.sample`，不代表任意采样调用已具备同样的会话保护。编辑器分配保活也不能替代这些回调各自的参数快照及返回后校验。

本批不改变曲线及 VMD 格式兼容规则，不开放普通生产宿主的 `mms.edit`。Win7 SP1 x64、CPU 基础路径、AMD 兼容及程序旁 `data` 的要求保持不变。正式 release 未更新。
