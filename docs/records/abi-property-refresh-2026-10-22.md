# 原属性自动刷新与共同历史通知

本批在 `codex/plugin-abi` 的 `035101d` 基础上增加 `mms.ui.refresh.property` 1.0。插件通知自身已注册属性变化后，原生 INPUT 在原刷新队列消费时重新读取属性；宿主的真实共同历史成功发布也可触发同一读回。两套构建和专项、相邻回归均已通过，普通生产 `mms.edit` 仍关闭。

## 实现范围

公开协议及开发说明见[原属性刷新参考](../../sdk/mms/property-refresh.md)。原 `mms.ui`、`mms.ui.refresh` 及其他 1.0 函数表布局不变。新表为 64 字节，提供能力、属性引用、订阅、通知、查询和注销六项函数。C 与 C++ 共同检查结构尺寸和偏移；C++17 SDK 组合原目标和原属性引用，默认 `DATA|REPAINT`、`flags=0`。

原 `PropertyRecord` 增加独立登记代次。`Manager` 在查询服务及插件 create 前精确绑定 `RefreshClient` 与 `UiClient`；不能通过相同插件 ID 转移所有权。属性同句柄替换、工程代次切换、注销和停止均使旧引用及订阅失效。

每个 INPUT 最多一个订阅，每插件最多 128 个、全局 2,048 个。通知邮箱有界，每个订阅最多一个内部票据；未提交的通知合并为最近序号，不保存通知时刻的属性值。消费继续经过原 SELF DATA 和 `NativePropertyInput::Readback`，活动草稿、起始规范值、焦点、选择及组合输入状态不被覆盖。

显式 DATA binding 存在时优先，原属性订阅停泊；真实解绑事件恢复当前值读回。新增绑定同时撤回未消费的默认属性贡献。该过程不轮询绑定状态，也不增加另一套 UI 或刷新队列。

`SharedHistory::SetPublicationSignal` 安装内部可信通知。成功的原生、Applied 和插件发布、撤销、重做及不记录步骤的 Adjustment 均在 revision 推进后发一次信号；失败、准备、取消和工程重置不发值通知。信号只记录原子状态并投递现有 Manager 消息，不分配应用堆内存、不读取插件或遍历注册表。UI 安全点再展开订阅和读取属性。关闭时先取消事务、解绑通知，再销毁借用上下文。

`WAIT_BINDING`、`WAIT_TARGET`、`WAIT_CAPACITY`、`WAIT_CONSUMER` 和 `WAIT_HOST_WAKE` 分别等待真实恢复事件。工作线程也检查原客户端的只读刷新因果，不能通过回调等待的 worker 反复通知。失败不因无关完成或重复 Drain 自动重试。

## 审查与失败记录

证据目录为 `build/verification/abi-property-refresh-20261022/`，下列初版日志和候选均保留。

- 独立审查发现 worker 通知绕过只读门禁，及历史 PostMessage 失败没有可查询状态。实现分别增加准确客户端的因果检查和仅原子操作的 `HistoryWakeResult`。新通知或真实成功唤醒恢复投递失败，停泊期间不启动轮询。
- `property-original-01` 在新测试第 441 项失败。测试沿用未绑定属性的 helper，错误要求显式 DATA 的 `sourceRevision=0`。改为严格核对显式来源版本，未放宽原协议。
- 运行中的目标从正尺寸变成零宽后，最终完成路径将 `NOT_READY` 误记为 `WAIT_CONSUMER`。`red-full-02` 实际记录 `result=-8 wait=8`，期望目标等待位 2。反例保持同实例，恢复发布不更换提供者，避免暂停状态掩盖缺口。修复在目标校验失败处记录原因并传至 Finish；提供者自身返回 `NOT_READY` 仍等待消费者恢复。最终该专项两套各 81 项通过。
- `property-full-03` 第 2,339 项在真实 NativeUi 订阅失败。原生 INPUT 呈现已带属性记录，但漏填内部控件类型，精确类型检查拒绝它。现在发布原 BUTTON、SEPARATOR、INPUT 的实际类型；不删除身份检查来绕过故障。
- `regressions-full-04` 第 27,199 项在既有来源订阅测试失败。新增绑定通知和旧解绑入口都执行清理，造成同一次容量释放重复唤醒。绑定变更只更新停泊，调用方统一执行一次清理；Drain 收尾保持原显式订阅的版本触发条件，再合并属性待处理状态。既有严格唤醒次数断言保持。

## DSH 子任务

使用已安装的 dsh-delegate 技能和当前真实宿主。sessionId 为 `5cbaabbc-6d4f-4535-b6ce-794bd9f953c1`，返回 cwd 为 `D:/MMD-Next/MMS-abi-dev`，来源绑定本会话真实线程。

DSH 仅修改 `tests/plugins/abi_c.c` 和 `tests/plugins/abi_layout.cpp`，分别追加 40、50 条编译断言及头文件 include，旧断言未删除。正式 result 为 `completed`、`sawError=false`。Codex 读取正式结果、核对 diff，再用两套配置实际编译验收。设计、文档和视觉工作均未交给 DSH；没有重启宿主或清理其记录。

## 最终验证

所有代码作者停止写入后构建。最终构建日志为 `build-full-05.log`、`build-original-05.log`，均 exit 0；完整配置包含 `plugin_abi_tests`、`native_edit_history_tests`、`side_mount_host_tests` 和产品，原始兼容配置包含 ABI 测试和产品。`candidate-05/manifest.json` 冻结 43 个源码、配置及二进制文件，后续运行使用这些产物。

最终运行报告为 `regressions-full-05/report.json` 和 `regressions-original-05/report.json`。17 组全部 exit 0，执行前后二进制 hash 一致。

| 检查 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| 运行中目标不可用及提供者 `NOT_READY` 区分 | 81 项 | 81 项 |
| 原属性 C ABI、SDK、NativeUi 和真实 DLL | 2,751 项 | 2,751 项 |
| 共同历史发布信号及无分配边界 | 184 项 | 不适用该完整配置目标 |
| 完整 ABI 契约 | 36,561 项 | 36,585 项 |
| DATA 及真实 DLL | 6,814 项 | 6,814 项 |
| 原来源自动订阅及真实 DLL | 6,096 项 | 6,096 项 |
| 左右挂载 | 3,159 项 | 3,159 项 |
| 暂停布局恢复 | 390 项 | 390 项 |
| 实际 MMS 普通及共同历史侧栏宿主 | 两种模式通过 | 不适用该完整配置目标 |

原属性专项包含通知突发合并、准确引用与同句柄换代、同 ID 不同 owner 拒绝、128 个订阅额度、真实队列满、64 贡献预算后已接受票据的绑定接管及生命周期取消、目标恢复和宿主唤醒失败。NativeUi 用原输入框验证规范读回、草稿及合成 IME 消息。C++ 假宿主只验证包装边界，另有真实 DLL 协商与 Manager 消息派发，未将手工 Drain 当作真实调度证明。

真实 DLL 的两组 Manager 均不挂 EditHost，严格核对 `mms.edit=NOT_FOUND`。41 次后台通知只读回最终值一次；普通命令通过公开 `ui.invoke` 通知。共同历史组真实提交、撤销、重做及 ExplicitOnly Adjustment 后，由发布信号更新同一 INPUT，不调用 NativeUi::Refresh 掩盖结果。停止时以真实 UI snapshot lease 保留 DLL，待处理订阅失效且不再读插件，释放后才卸载。

实际 MMS 的相邻普通、共同历史侧栏回归分别记录 246、366 次属性读取，各 7 个队列命令，`scene=0`、`physics=0`，实际场景、键池和历史不变量保持。该专项验证现有集成未回归，不代替新属性监听在任意真实场景中的全面验收。

`pe-products-final-05.json` 记录两套产品仍为 x64、子系统 6.1，直接和延迟导入与上一批一致，无新增或删除。完整配置产品 SHA-256 为 `19bbbde8ae61d3a0164dbc7c56977cc35cdf4ac7cb76d05d72f11db570d7ff51`，原始兼容产品为 `2ce50388d96a04119a60f5e26c3695bcd708ed3a8f65ebef4c411a2a8c8ba218`。正式 `build/release` 产品仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，未覆盖。

## 未完成范围

运行继承 Default 桌面，只操作夹具自身窗口，没有全局输入或桌面像素检查。显示由夹具控制；不能记为命名私有桌面、肉眼视觉、真实微软拼音、多屏 DPI、Win7 真机或 AMD 真机验收。构建仍有既有测试析构 C4297 等警告，不宣称零警告。

本批不改 PMM、VMD、VPD 标准数据布局或读取兼容规则，不改本地数据路径，不捆绑第三方插件。原始兼容产品仍保持其关闭插件运行时的启动规则；两套 runtime 测试通过不表示该产品已开放全部服务。

依赖图自动传播、更多挂载点、完整宿主语义登记、多视口与独立求值、格式编辑扩展及其他原生写入口仍待实现。此前曲线堆损坏、未解释全集超时及菜单偶发超时保持原记录，不能被本批通过结果关闭。MMS main 当前提交仍为 `c147454`，其中未提交修改由原工作区继续管理，本批未同步或改写。
