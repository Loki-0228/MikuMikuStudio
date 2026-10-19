# 第三方依赖许可

本目录保留新增依赖的上游许可文本。分发主程序时应一同分发本目录。

| 依赖 | 版本或来源 | 许可文本 |
| --- | --- | --- |
| Alembic | 1.8.8 | [BSD-3-Clause](Alembic-LICENSE.txt) |
| Imath | 3.2.2，Alembic 的传递依赖 | [BSD-3-Clause](Imath-LICENSE.md) |
| MinHook | 1.3.4 | [BSD-2-Clause 及附带反汇编器声明](MinHook-LICENSE.txt) |
| SMAA | 上游提交 `71c806a838bdd7d517df19192a20f0c61b3ca29d`，随源码提供 | [SMAA 许可](SMAA-LICENSE.txt) |
| FXAA | NVIDIA FXAA 3.11 PC Quality，经 MMS 适配，随源码提供 | [NVIDIA 许可](FXAA-LICENSE.txt) |

原有 Bullet、DirectX SDK 与 Windows SDK 的许可不变。Conan 依赖版本由 `conanfile.py` 和 Conan 生成器记录；SMAA 和 FXAA 来源分别见 `third_party/smaa/UPSTREAM.txt`、`third_party/fxaa/UPSTREAM.txt`。
