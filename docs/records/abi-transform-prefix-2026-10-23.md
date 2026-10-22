# 长变换数值的前缀显示

2026-10-23。ABI 基线为 `084699a`，开始及完成构建时核对 main 为 `fed9768f9fe726c4b5e068849808803f1713ad42`。行为来源是 main `3782e0c` 中的长输入框修复，只读取已提交内容。

## 实际行为

增强版的附件位置、旋转和缩放，相机及骨骼位置、旋转、距离，以及骨骼缩放框改为左对齐。目标 ID 为 478–484、544–550、572、575、577，共 17 个。透明度 485、表情权重及其他有界输入框保留原对齐方式。原始兼容配置不启用该显示修复。

目标框失去焦点，或在未聚焦时收到 `WM_SETTEXT`、`EM_REPLACESEL` 后，显示位置回到文本开头，以便窄框仍能显示负号和整数前缀。聚焦期间的选区、键入及横向滚动继续使用原生 EDIT 行为。修改不更改文字、数值解析或工程数据，也不新增历史操作。

`CreateUiPanelControl` 的 ANSI 和 Unicode 创建路径使用同一 ID 判断。`EditSubclassProc` 保留已有 Enter、失焦提交、草稿 modify 标志及播放门禁的顺序。新增后处理在转发消息前保存原窗口过程，回调后检查宿主、父窗口、控件 ID、实际窗口过程、线程及焦点。发送选区消息后再次检查，才滚动文本。线程内的栈链阻止同一控件的显示后处理递归进入，其他控件仍可处理自己的更新。

独立审查发现初稿缺少宿主身份比较，现已在两次有效性检查中加入 `g_Block == app`。已有提交函数内部的任意销毁场景，以及相同地址和句柄精确复用，不在本次保证范围内。

## 实现与分工

- `src/window/transform_edit_layout.hpp`：限定目标及配置。
- `src/window/ui_init.cpp`：只清除目标框的居中和右对齐样式。
- `src/window/ui_edit_commit.cpp`：保留原提交路径，追加受保护的显示更新。
- `tests/transform_edit_prefix_tests.cpp`：实际生产工厂和子类过程的独立专项。

DSH 通过已安装的 `dsh-delegate` skill 完成前两项原子修改。真实会话为 `083427ff-6820-4a52-aded-f0a371213ce4`，返回 cwd 为 `D:\MMD-Next\MMS-abi-dev`，正式结果为 completed。Codex 完成后处理、测试、审查、运行及文档；DSH 的完成状态没有代替验收。

## 验证

证据目录为 `build/verification/abi-transform-prefix-20261023`。运行器复制测试程序到独立安装目录，继承 Default 桌面，串行操作自有隐藏窗口；不调用全局键鼠输入。测试没有证明用户前台与捕获状态的所有间接影响。

| 验证 | 结果与证据 |
| --- | --- |
| 增强和原始兼容产品、专项构建 | `build-full-restored.json`、`build-original-restored.json` 均退出 0。 |
| 增强版长数值专项 | `prefix-full-04` 及 `prefix-full-restored` 各 12,153 项通过。 |
| 原始兼容版专项 | `prefix-original-04` 及 `prefix-original-restored` 各 11,843 项通过。 |
| 数值提交回归 | `numeric-full-01`，310 项通过。 |
| 绑定旋转重置回归 | `rotation-full-01`，826 项通过。 |
| 普通旋转重置回归 | `unbound-full-01`，6,597 项通过。 |
| 原对齐反例 | 临时禁用目标判断，`counter-alignment` 在独立 ID 对齐断言失败，退出 1。 |
| 缺后处理反例 | 临时禁用新增后处理，`counter-postprocess` 在非聚焦前缀断言失败，退出 1。 |
| 恢复证明 | `candidate.json` 记录五个代码及测试文件哈希；反例脚本逐项恢复原字节，恢复后重建并重跑两套专项。 |
| 独立审查 | `review-production.txt`、`review-tests.txt`。 |
| 产品兼容边界 | `pe-check-final.json`：两产品为 x64、子系统 6.1，导入表无增减，效果资源 117、118 不变。 |

专项独立枚举 17 个目标及 9 个非目标，覆盖主窗口和分离窗口，使用三档字体及窄宽度模拟 100%、175%、200% 布局。聚焦行为与同样式原生 EDIT 逐项对照，失焦后检查首字符实际位于可见矩形内。重入场景包含窗口销毁、换父、换窗口过程、重新聚焦、转移至其他自有窗口，以及宿主清空或替换。`FrameStepPlayback=1` 隔离真实提交，专项只证明显示与有限状态哨兵；提交和撤销证据来自相邻回归。

首次运行失败记录保留。`prefix-full-01` 在无 v6 激活上下文时初始化 `ICC_STANDARD_CLASSES` 失败；专项仅用 USER32 的 EDIT 和 STATIC，已移除无关初始化。`prefix-full-02` 及 `prefix-full-diag` 揭示单行 EDIT 的行查询返回 0，但首字符横坐标为 -620，原测试错误地只用行查询判断横向滚动。`prefix-full-03` 进一步证明 `EM_REPLACESEL` 后生产框和原生框的末字符横坐标均为 60，超出宽度 36；程序替换文本不保证立即显示新增尾字符。最终测试保留完整原生对照，仅对真实 `WM_CHAR` 键入另行要求末字形可见。没有为通过这些测试而修改生产代码。

最终增强产品 SHA256 为 `b8d4e210a9d9f54db8ec43a2c5fb970e080415b994cb1c393cea09bf2e40c9b2`，原始兼容产品为 `94dab7679add7855f48cdd6f72ff5a42e5b52348d92bfe76f819d113622af4c9`。稳定版 `build/release/MikuMikuDance.exe` 保持 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

## 尚未覆盖

本批没有真实多屏 DPI、像素截图、Win7 或 AMD 真机验收。专项没有直接触发递归文本更新栈链、控件 ID 变化或同值句柄复用，也不代表全部 UI 生命周期已验证。VMD、PMM 和 GPU 路径没有改动。菜单首次绘制、磁盘缓存管理、负帧及其他 ABI 缺项继续保留，普通公共 `mms.edit` 仍关闭。
