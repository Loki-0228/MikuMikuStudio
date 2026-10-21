# 曲线测试按构建配置注册

原始兼容配置的 `mmd932.cmake` 排除了 `animation_curve_host.cpp`、`bone_curve.cpp`、`morph_curve.cpp`、`animation_curve_host_ui.cpp`、`native_frame_retime.cpp` 和 `bone_scale.cpp`，由原始 provider 明确禁用相应扩展。此前 CMake 仍无条件声明这些扩展的测试，选择 `bone_curve_tests` 构建会产生 18 个未解析符号。

现在只在 full 配置声明并注册这六项测试：`animation_curve_host_tests`、`bone_curve_tests`、`morph_curve_tests`、`native_frame_retime_tests`、`morph_curve_host_ui_tests`、`bone_scale_curve_tests`。没有删除原始兼容配置的生产功能，也没有添加空实现或修改测试断言。

独立数学、motion、comparison、editor、sidebar、retime 和帧率测试保留两套配置；帧率测试自行编译被测实现及独立后端，不依赖原始 provider 已禁用的扩展。

证据目录为 `build/verification/abi-profile-curve-tests-20261021`。

| 检查 | 结果 |
| --- | --- |
| full 六个测试目标 | `build-full-01.log` 构建成功，退出 0。该批未重复运行这些未修改的测试。 |
| original 产品及共同目标 | `build-original-01.log` 构建成功，退出 0。 |
| 实际 CTest 注册 | original 从 222 项变为 216 项，准确移除上述六项；full 仍含这些目标，共 274 项。两边均保留九个核对的共同入口，见[注册核对](../../build/verification/abi-profile-curve-tests-20261021/registration-check.json)。 |
| 产品文件 | 与前批三个产品的 SHA256 全部相同。本批只修改构建测试范围。 |

本记录不宣称全部注册测试通过，也不关闭原始兼容曲线全集的 `0xC0000374`。该故障继续按[插帧寿命记录](abi-curve-subdivision-lifetime-2026-10-21.md)调查。
