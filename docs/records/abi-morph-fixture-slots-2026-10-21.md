# 表情曲线测试的模型槽初始化

`morph_curve_tests` 的两个夹具只写前两个模型槽，但 `MMDApp` 的还原构造不初始化旧状态块。PMM 捕获、加载和 Reset 会扫描完整模型槽表，因此未填写的槽也必须明确为空。

`PmmRoundTrip` 和 `DenseWriteAndGrowthIdentity` 现在先调用 `ClearModelSlots`，再填写所需模型及当前选择。没有清零包含 C++ 成员的整个对象，没有修改产品或断言。

证据在 `build/verification/abi-curve-subdivision-20261021`。完整配置增量构建成功，工作区 TEMP/TMP 和 Default 桌面下[211 项通过](../../build/verification/abi-curve-subdivision-20261021/morph-fixture-full-04/report.json)。该修正来自静态发现的未初始化读取；不能把此前系统临时目录运行的 `0xC0000409` 直接归为此问题。原二进制在工作区临时目录下、修改夹具前也通过了 211 项，见[对照报告](../../build/verification/abi-curve-subdivision-20261021/morph-workspace-full-03/report.json)。
