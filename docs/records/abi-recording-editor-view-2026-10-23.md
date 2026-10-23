# 导出结束恢复自由编辑视角

本批同步 main 中导出结束恢复自由模型编辑视角的行为，并补充 ABI 分支的场景代次、父对象身份及启动重入保护。增强配置和原始配置的产品构建、字段检查、窗口化 BMP 生命周期及录制回归均已通过。本记录只确认下列验收范围，不代表完整导出和 ABI 需求全部完成。

## main 行为核对

已读取 main 的提交对象 `3782e0c` 和当前 main `fed9768`。`3782e0c` 已包含 `CaptureRecordingEditorView`、`RestoreRecordingEditorView` 及两个录制启动入口的调用。`fed9768` 保留该实现，结束顺序为帧求值、恢复视角、重建投影、刷新读数。

ABI 本批依据这些行为实现，未整批合并 main。原有同步状态曾遗漏这项功能，ABI 基线 `7408a45` 已记录缺口。本记录只覆盖该缺口，不代表 `3782e0c` 中所有功能均已同步。

## 恢复的数据与适用模式

`MMDApp` 的快照仅在 `CameraMode()==0` 且 `followCameraEnabled==0` 时捕获自由模型编辑视角。相机模式与跟随模式不捕获快照，继续原有导出结束行为。

快照保存以下运行时字段：

- 相机位置、旋转、平移偏移、距离。
- FOV、透视开关。
- 父模型槽、父骨骼下标、参考模式、附件变换基底及抑制标记。
- 捕获时的场景代次和父对象身份。

这些字段存于 `MMDApp` 私有运行时成员，不写入 PMM，也不增加工程编辑或撤销事务。本批不恢复导出前的播放头，不还原模型姿态。单张图片继续用独立的 `CancelStillCapture` 清理，不进入录制结束的帧求值流程。

## 父绑定与场景寿命

`CameraParentModel` 指向模型槽，`CameraParentBone` 指向模型内的骨骼。快照保存父模型指针、稳定身份的 `id` 与 `generation`、骨骼表地址及数量。缺少已登记身份时，捕获尝试登记；分配失败只禁用父绑定恢复，不向外抛出异常。

恢复先核对 `SceneGeneration`，再核对槽中的当前指针和稳定身份，最后检查骨骼表及下标。恢复不会先解引用快照中的旧指针。父对象已删除、槽位被复用或骨骼表改变时，恢复自由视角数值，同时解除父绑定并重置基底。

原始配置也会在 `ForgetSceneObject` 中执行 `identities.Forget(token)`，使删除后复用同一指针的对象获得新身份。该分支随后直接返回，不进入增强配置的几何摘要和场景 revision 更新。

`ResetAppState` 和 PMM 的 `AcceptProjectSceneReplacement` 已推进场景代次。旧场景快照不能恢复到新工程。快照只消费一次，后续结束调用不会再次用它覆盖用户修改。

## 启动与取消的顺序

窗口和全屏入口共用 `Operation::Start`，禁止同一应用重复启动。启动范围用 `WindowStopGuard`，主窗口关闭请求延至退出该范围后投递。主窗口和分离窗口在启动、结束或图清理期间拒绝编辑输入。

每次启动保存 token 和场景代次。窗口显示、视口刷新、图构建、停放主窗口以及启动末尾的刷新返回后，入口核对 token、场景和录制窗口。已结束的旧调用不能继续隐藏主窗口、再次创建图或设置 `recordPlaybackStartPending`。

`GraphBuild` 覆盖普通编码图构建及图片序列启动前的旧图清理。图构建期间收到 `FinishAviRecord` 时，guard 只记录取消请求。构建函数返回并离开 guard 后才执行结束流程，避免在构建函数仍借用 COM 对象时释放对象。多次取消合并为同一个请求。

`RecordingPlaybackEntered` 区分资源准备与帧求值。启动时该值为 `false`，`RecordStartTail` 在首次求值前置为 `true`。窗口创建、设备准备或图构建失败时，结束流程仍释放资源，但不调用 `StopPlayback` 和 `RefreshAfterFrameApply`，避免导出尚未开始就覆盖未注册姿态。没有启动 token 的既有结束调用保持原语义。

## 结束恢复与新场景保护

`FinishAviRecord` 在失效 token 前保存本次启动的场景代次。只在同一场景中停止播放、重新求值并恢复快照。资源清理期间换了场景，后续编辑刷新会跳过。

同场景的恢复顺序如下：

1. 已进入播放阶段时，执行原有停止播放和帧求值。
2. 消费快照并恢复自由编辑视角。
3. 清除抗锯齿历史。
4. 设备不处于丢失等待状态时，刷新主视口；重新核对场景和设备状态后，调用 `ApplyFrameCameraProjection` 应用恢复的 FOV 与透视设置。
5. 刷新界面读数。

跨场景取消仍释放图、图片序列会话、透明捕获、录制窗口和捕获资源，并清除录制运行态。旧 `AviSavedRenderWidth/Height` 不写入新工程。此时预期完成帧数为 `0`，不以新工程的输出区间判断旧导出完成，也不触发导出后关闭程序。

## 实际验证

证据目录为 `build/verification/abi-recording-editor-view-20261023/`。下表为实际断言检查计数，不是固定测试用例数。各运行目录的 `report.json` 均记录退出码 `0`，`output.log` 保存实际结果。

| 目标或入口 | 增强配置 | 原始配置 | 运行目录 |
| --- | ---: | ---: | --- |
| `recording_state_tests` | 88 | 88 | `state-full-01`、`state-original-01` |
| `recording_teardown_tests` | 63 | 63 | `teardown-full-final`、`teardown-original-01` |
| `recording_editor_view_tests --fields` | 56 | 56 | `fields-full-01`、`fields-original-01` |
| `recording_editor_view_tests --lifecycle` | 1028 | 975 | `lifecycle-full-identity-final`、`lifecycle-original-identity-fix` |

字段检查覆盖独立字段预期、重复捕获、一次消费、模式排除、场景代次及多应用隔离。生命周期检查加载本地生成的 PMD 模型，执行真实 BMP 序列完成与取消，并检查恢复后的字段和实际 D3D9 投影矩阵。

生命周期中的注入用例覆盖图失败与延期取消、视口刷新和启动末尾回调、场景代次改变、父身份失效和设备 Reset 失败。`EarlyWindowFailure` 定点拒绝本次录制的窗口创建，验证结束流程释放资源且未调用 `StopPlayback` 或 `RefreshAfterFrameApply`。它还检查未注册骨骼姿态、视角、播放头和工程标记未被改变。此用例不等于已验证所有早期失败路径，`GrowRenderTarget` 失败仍未单独注入。

生命周期测试要求新建隔离安装与输出目录，拒绝已有数据和插件安装。测试只控制自身线程与隐藏窗口，不发送全局输入。顶点计算指定 CPU 路径，但生命周期仍创建真实 D3D9 设备，不能称为纯 CPU 测试。

图构建故障及回调场景用本进程定点 hook 注入。场景更替用 `AdvanceSceneGeneration` 加人工设置的新场景字段验证，不是实际加载 PMM 或执行完整工程替换。设备丢失用自身设备的 Reset 返回值注入。这些检查不等同真实编码器故障或真实硬件设备丢失。序列文件数量和投影矩阵断言也不等同最终像素一致。

## 实测失败与修复

首次增强配置生命周期在第 518 次检查失败，断言为 `projection already restored before the next frame`。`lifecycle-full-projection-diagnostic` 确认正交投影在结束后变成透视投影：视角字段虽已恢复，主视口刷新仍覆盖了矩阵。结束流程现在于该刷新之后调用 `ApplyFrameCameraProjection`。修复后的 `lifecycle-full-02` 通过 987 次检查；加入早期窗口失败用例及最终身份修复后，增强配置通过 1028 次检查。

首次原始配置生命周期在第 262 次检查失败，断言为 `same pointer now has a distinct stable object identity`。原始配置的 `ForgetSceneObject` 原先直接返回，录制快照新增的运行时身份没有随对象删除失效。补充 `identities.Forget(token)` 后，原始配置通过 975 次检查。最终 `scene_host.cpp` SHA256 为 `8c82843041bef37a866a6c302bfed9b51cfe4247d2f4fed4c65f89dd8be5b03c`。

上述两项是实际运行发现并修复的失败，没有为录制功能另做移除修复的受控反例。缓存指定身份初始化的受控反例单独记录在[对应批记录](abi-cache-owned-initialization-2026-10-23.md)。

## 产品与平台核对

`build-full-identity-fix.json` 和 `build-original-identity-fix.json` 记录最终两套产品及生命周期目标构建成功。`product-pe-final.json` 记录两套产品均为 x64、子系统版本 `6.1`，效果资源 117、118 与各自基线一致。核对脚本还确认稳定版程序哈希未变，main 仍为 `fed9768`。

增强配置的导入无增减。原始配置新增唯一普通导入 `bcrypt.dll!BCryptGenRandom`，没有移除导入；原因是录制父对象快照现在调用已有的场景身份生成器。首次 PE 检查要求两套导入严格不变，因此失败。核对新增导入后，检查仅允许原始配置增加该符号，最终通过。

微软文档列出的 `BCryptGenRandom` 最低客户端为 Windows Vista，当前所用 `BCRYPT_USE_SYSTEM_PREFERRED_RNG` 在 Vista 上要求 SP2。这项导入不提高 Win7 SP1 的 API 最低要求，但静态导入检查不能替代 Win7 真机验收。[官方 API 要求](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom)

## 验收边界与证据

全屏 3D Vision、真实 H.264 和 DirectShow 编码器、第三方 MME、Win7 真机、AMD 真机均未取得本批通过证据。相机模式与跟随模式由字段级排除用例检查，不能据此宣称其完整导出流程已验收。

父身份检查不覆盖保持同一身份、同一骨骼表地址和数量的任意原地骨骼重排。本批也不承诺原生插件可在任意回调中销毁或替换整个应用及 recorder 后，原调用仍安全继续。

`design-review.txt` 和 `recording-final-review.txt` 保留发现的问题、修复过程及静态结论。`view-test-review.txt` 记录测试设计审查。

DSH 完成了初始四文件修改，正式结果保存在 `dsh-result-final-reentry.txt`。Codex 随后补充 `GraphBuild` 延期取消、窗口忙碌处理、早期阶段判断及实际失败修复。DSH 的 `completed` 仅表示委派轮次结束，最终整合版的验收依据是本页列出的构建和运行报告。
