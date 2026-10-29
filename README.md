# MikuMikuStudio (MMS)

[English](#english) | [中文](#中文) | [日本語](#日本語)

[Release notes / 发布日志 / リリースノート](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.20.1)

[Documentation / 文档导航 / ドキュメント](docs/README.md)

[v2026.10.20.1 Release / 正式版 / 正式版](docs/records/releases.md#release-v2026-10-20-1)

## Downloads and package differences

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.20.1) provides Windows x64 packages with `full` or `minimal` in the filename and lists their contents in the release notes. The table below describes the released build profiles. The minimal build uses the `mmd932` build profile:

| Area | Full `full` | Minimal `minimal` |
| --- | --- | --- |
| PMD and PMX loading, original animation tools, rigid-body physics, language fixes, MME interface | Included | Included |
| Image sequences, periodic backups, crash backup attempts, and morph force paste | Included | Included |
| Model preview optimizations | Included | Included |
| MME effect auto-reload | Included | Included |
| Preview shadow resolution | Scales the built-in renderer's shadow map; MME-owned targets follow their FX declarations | No preview scaling |
| Recent projects, timeline force-paste button, and viewport render scaling | Included | Omitted |
| FXAA, TAA, SMAA | Included; NVIDIA fix DLL is a separate optional download | Original anti-aliasing switch only |
| New transparency sorting | Optional, disabled by default | Omitted; original single-pass material order |
| Shadow reconstruction, dynamic precision, filtering | Extended shadow controls; optional filtering, disabled by default | Original shadow behavior |
| Bone scaling, nine-channel bone curves, and morph curves | Included | Omitted; original Bezier interpolation retained |
| DX12, mesh capture, Alembic, additional baking | Included | Omitted |
| Media | Modern imports and MP4/MOV export through Windows media components | WAV audio, AVI backgrounds and AVI export |
| PMM, VMD, VPD saving | Can include MMS extensions | Standard data only; imported MMS extensions are ignored |

Both application ZIPs contain `MikuMikuDance.exe` and licenses. Neither includes models, user settings, MME, or the NVIDIA fix DLL. Starting with `v2026.10.15.1`, `MMSNvidiaAAFix.dll` is a separate optional release asset with [installation instructions in the release notes](docs/records/releases.md#optional-nvidia-fix).

The minimal build retains the original feature range with selected fixes; it is not an unchanged copy of original MMD. PMX models can be animated, but model editing supports PMD only. Import success does not guarantee lossless saving in legacy formats. Save extended projects as separate copies, since the minimal build drops MMS extensions. See [minimal build limits](docs/reference/minimal-build.md) and [tested legacy boundaries](docs/reference/legacy-mmd-limits.md).

Extract the two builds into separate directories and run the executable in each directory. Keep your existing installation if it contains settings or plugins.

### PMM and VMD files

PMM saves a project, including models, media references, scene settings, and keyframes. VMD saves motion to apply to another model or project; it does not replace a PMM.

| Saved content | Original MMD compatibility |
| --- | --- |
| Standard PMM `0002` and VMD motion | Preserve data that fits the original name, path, frame, and keyframe limits. Identical results for every project and third-party reader are not guaranteed. |
| Complete names in MMS extensions | Original MMD reads the standard name fields, not the full-name extension. Loading a Unicode-named PMX does not mean its names can be saved unchanged in legacy PMM or VMD. |
| Negative keyframes | The full build saves them in the `MMSXPMM2` extension of PMM `0002` or `0003`. Negative frames alone do not require `0003`. Original MMD cannot restore them; standard VMD export contains only nonnegative keyframes. |
| Bone scale, free curves, and mesh caches | MMS extensions require MMS. Original MMD reads only standard data and cannot reproduce these additions unchanged. |
| FXAA, TAA, SMAA, and other MMS display settings | Apply in MMS. Original MMD uses its own rendering settings. |
| Extended PMM `0003` | Original MMD and the minimal build cannot open it. Actual excess keyframes, display counts, or frame numbers, can require this format. Reference paths use a reversible byte encoding when they fit the field. |

The full build prefers `0002` when the standard project data still fits. Empty reserved capacity, name extensions, and negative keyframes alone do not select `0003`. When usable, a legacy-name copy is generated automatically as `.compat.pmm` or `.compat.vmd`; excess capacity or conflicting short names can prevent it. A standard-only PMM copy is not generated when it would discard negative-frame data. These copies cannot add MMS-only features to original MMD.

Re-saving in original MMD or the minimal build discards MMS extensions. Keep a separate copy. See [PMM save rules](docs/reference/pmm-extensions.md) and [measured legacy limits](docs/reference/legacy-mmd-limits.md) for boundaries and verification scope. A new PMM section layout is a design document, not the current save format.

## 下载与版本区别

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.20.1) 同时提供 Windows x64 完整包和最小包，实际收录内容见发布页面的日志。需要 MMS 新增功能时选择文件名含 `full` 的 ZIP；主要使用 MMD 原有功能时选择含 `minimal` 的 ZIP。下表描述已发布的两种构建档，两档均保留 `MikuMikuDance.exe` 文件名。

| 项目 | 完整包 `full` | 最小包 `minimal`（构建档 `mmd932`） |
| --- | --- | --- |
| PMD、PMX 载入，原有骨骼、表情、相机、灯光和刚体物理 | 保留 | 保留 |
| 原生功能、输入、编码及 MME 兼容修复 | 保留 | 保留 |
| 图片序列、周期备份、崩溃备份尝试、表情强制粘贴 | 保留 | 保留 |
| 模型预览计算优化 | 保留 | 保留 |
| MME 效果自动重载 | 保留 | 保留 |
| 预览阴影分辨率 | 只调整默认渲染的阴影贴图；MME 自建目标遵循 FX 声明 | 不包含预览缩放 |
| 最近工程、时间轴强制粘贴按钮、视口渲染比例 | 保留 | 不包含 |
| FXAA、TAA、SMAA | 保留；NVIDIA 补丁 DLL 另行按需下载 | 仅保留原抗锯齿开关 |
| 新透明排序 | 可选强度，默认关闭 | 使用原版材质顺序 |
| 阴影重构、动态精度和滤波 | 扩展阴影控制；可选滤波，默认关闭 | 使用原版阴影行为 |
| 骨骼缩放、九通道骨骼曲线和表情曲线 | 保留 | 不编入，保留原有 Bezier 插值 |
| DX12、通用 mesh 捕获、Alembic 和新增烘焙 | 保留 | 不编入对应实现 |
| 音视频 | 保留现代音视频导入与 MP4/MOV 输出，依赖 Windows 媒体组件 | WAV 音频、AVI 背景与 AVI 输出 |
| PMM、VMD、VPD 保存 | 可写 MMS 扩展数据 | 仅写标准内容，忽略读取到的 MMS 扩展 |
| MME | 保留原生 DX9 接口，插件需另行安装 | 保留原生 DX9 接口，插件需另行安装 |

完整包和最小包 ZIP 均包含 `MikuMikuDance.exe` 与许可文件，不附带用户配置、模型、MME 或 NVIDIA 修复 DLL。从 `v2026.10.15.1` 起，`MMSNvidiaAAFix.dll` 作为独立附件按需下载，见[Release Notes 中的安装说明](docs/records/releases.md#nvidia-修复安装可选)。

最小包保留原版功能范围及部分修复，不等同于原版程序。PMX 可用于动画场景，模型编辑仅支持 PMD。导入成功不代表能以旧格式无损保存。最小包保存时会丢弃 MMS 扩展，请将扩展工程另存为副本。具体限制见[最小包范围](docs/reference/minimal-build.md)及[原版限制实测](docs/reference/legacy-mmd-limits.md)。

两档解压到不同目录，分别运行各自的 `MikuMikuDance.exe`。有旧版配置和插件时，先保留旧目录；不要把两个 ZIP 解压到同一目录后相互覆盖。

### PMM 与 VMD 的区别和兼容性

PMM 保存工程，包括模型与媒体引用、场景设置和关键帧。VMD 保存动作，便于应用到其他模型或工程，不能代替 PMM。

| 保存内容 | 原版 MMD 的兼容范围 |
| --- | --- |
| 标准 PMM `0002`、VMD 动作 | 可保留原版名称、路径、帧号及关键帧容量范围内的数据，不保证任意工程或第三方读取器的结果完全一致。 |
| MMS 扩展中的完整名称 | 原版只读标准名称字段，不恢复完整名称扩展。能载入 Unicode 名称的 PMX，不等于能在旧 PMM、VMD 中原样保存名称。 |
| 负数关键帧 | 完整包保存到 PMM `0002` 或 `0003` 的 `MMSXPMM2` 扩展区。负帧本身不要求 `0003`。原版无法恢复负帧，标准 VMD 导出只包含非负关键帧。 |
| 骨骼缩放、自由曲线、网格缓存 | 依赖 MMS 扩展。原版只读取标准部分，不能原样还原这些新增效果。 |
| FXAA、TAA、SMAA 等新增显示设置 | 只在 MMS 中生效，原版使用自己的渲染设置。 |
| 扩展 PMM `0003` | 原版 MMD 和最小包均不能打开。实际关键帧数量、显示项数量、帧号超限，时，可能需要此格式。引用路径在字段可容纳时采用可还原的字节编码。 |

完整包在标准部分符合原版限制时优先保存为 `0002`。仅预留更多空容量、增加名称扩展或负数关键帧，不会切换到 `0003`。能够生成可用的旧名称副本时，自动写入 `.compat.pmm`、`.compat.vmd`；超容量或短名称冲突时可能无法生成。含负帧数据时不生成会丢失该数据的标准部分 PMM 副本。这些副本不能让原版获得 MMS 专有功能。

原版 MMD 或最小包重新保存时会丢弃 MMS 扩展，请保留独立副本。详细边界和验证范围见 [PMM 保存规则](docs/reference/pmm-extensions.md)及[原版限制实测](docs/reference/legacy-mmd-limits.md)。新的 PMM 分节布局仍是设计文档，尚未用于实际工程保存。

## ダウンロードとパッケージの違い

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.20.1) では、ファイル名に `full` または `minimal` を含む Windows x64 パッケージを配布しています。収録内容は公開ページのリリースノートを参照してください。下表は公開済みのビルド構成です。最小版は `mmd932` ビルドプロファイルを使用します。

| 項目 | 完全版 `full` | 最小版 `minimal` |
| --- | --- | --- |
| PMD・PMX 読み込み、従来のアニメーション編集、剛体物理、言語関連の修正、MME インターフェース | 含む | 含む |
| 連番画像、定期バックアップ、クラッシュ時のバックアップ試行、モーフの強制貼り付け | 含む | 含む |
| モデルのプレビュー計算の最適化 | 含む | 含む |
| MME エフェクトの自動再読み込み | 含む | 含む |
| プレビューのシャドウ解像度 | 標準レンダラーのシャドウマップのみ調整。MME 独自のターゲットは FX の宣言に従う | プレビュー縮小なし |
| 最近使ったプロジェクト、タイムラインの強制貼り付けボタン、ビューポート描画倍率 | 含む | 含まない |
| FXAA、TAA、SMAA | 含む。NVIDIA 修正 DLL は任意の別ダウンロード | 従来のアンチエイリアス切り替えのみ |
| 新しい透明ソート | 任意で有効化、初期設定は無効 | 含まない。従来の単一パスの材質順序を使用 |
| シャドウ処理の再構成、動的精度、フィルタリング | 拡張シャドウ設定、任意のフィルター（初期設定は無効） | 従来のシャドウ処理 |
| ボーンスケール、9 チャンネルのボーン曲線、モーフ曲線 | 含む | 含まない。従来の Bezier 補間を維持 |
| DX12、メッシュ取得、Alembic、追加のベイク機能 | 含む | 含まない |
| 音声・動画 | Windows メディアコンポーネントによる各種形式の読み込みと MP4・MOV 出力 | WAV 音声、AVI 背景、AVI 出力 |
| PMM・VMD・VPD 保存 | MMS 拡張を保存可能 | 標準データのみ保存。読み込んだ MMS 拡張は無視 |

両方のアプリ ZIP に `MikuMikuDance.exe` とライセンスを含みます。モデル、ユーザー設定、MME、NVIDIA 修正 DLL は同梱しません。`v2026.10.15.1` 以降、`MMSNvidiaAAFix.dll` は任意の独立ダウンロードです。[リリースノートの導入手順](docs/records/releases.md#nvidia-修正の導入任意)を参照してください。

最小版は従来の機能範囲と一部の修正を保持しますが、オリジナル MMD そのものではありません。PMX はアニメーションに使えますが、モデル編集は PMD のみです。読み込みに成功しても旧形式で無損失に保存できるとは限りません。MMS 拡張は最小版で保存すると失われるため、別名で保存してください。[最小版の範囲](docs/reference/minimal-build.md)と[MMD の実測記録](docs/reference/legacy-mmd-limits.md)を参照してください。

両方を使う場合は別々のフォルダーに展開し、それぞれの実行ファイルを起動してください。設定やプラグインが入った既存のフォルダーは残してください。

### PMM と VMD の違いと互換性

PMM はモデルやメディアの参照、シーン設定、キーフレームを含むプロジェクトを保存します。VMD は別のモデルやプロジェクトに適用するモーションで、PMM の代わりにはなりません。

| 保存内容 | オリジナル MMD の対応範囲 |
| --- | --- |
| 標準 PMM `0002`・VMD モーション | 従来の名前・パス・フレーム・キー容量の範囲に収まるデータを保持します。任意のプロジェクトや他の読み込みソフトで同じ結果になる保証ではありません。 |
| MMS 拡張の完全な名前 | オリジナル MMD は標準の名前欄のみ読み、完全な名前の拡張は復元しません。Unicode 名の PMX を読み込めても、従来の PMM・VMD に名前をそのまま保存できるとは限りません。 |
| 負のキーフレーム | 完全版は PMM `0002` または `0003` の `MMSXPMM2` 拡張に保存します。負のフレームだけでは `0003` に切り替わりません。オリジナル MMD では復元できず、標準 VMD 出力には非負のキーのみ含まれます。 |
| ボーンスケール・自由曲線・メッシュキャッシュ | MMS 拡張には MMS が必要です。オリジナル MMD は標準部分のみを読み、追加した効果をそのまま再現できません。 |
| FXAA・TAA・SMAA などの追加表示設定 | MMS で適用します。オリジナル MMD は自身の描画設定を使います。 |
| 拡張 PMM `0003` | オリジナル MMD と最小版では開けません。キー数・表示項目数・フレーム番号の超過で必要になる場合があります。参照パスは、フィールドに収まれば復元可能なバイト表現で保存します。 |

完全版は標準部分が従来の制限内なら `0002` を優先します。空き容量の確保、名前の拡張、負のキーフレームだけでは `0003` に切り替わりません。旧形式の名前を安全に扱える場合は `.compat.pmm`・`.compat.vmd` を自動保存しますが、容量超過や短縮名の重複で作れない場合があります。負のフレームがある場合、そのデータを失う標準部分のみの PMM コピーは生成しません。互換コピーで MMS 固有の機能がオリジナル MMD に追加されるわけではありません。

オリジナル MMD や最小版で再保存すると MMS 拡張は失われるため、別のコピーを残してください。境界と検証範囲は [PMM の保存規則](docs/reference/pmm-extensions.md)と[MMD の実測記録](docs/reference/legacy-mmd-limits.md)を参照してください。新しい PMM のセクション構造は設計段階で、通常保存にはまだ使われていません。

## English

MikuMikuStudio (MMS) is a Windows desktop application for creating and editing MMD dance animations. The following features describe the current full-build source. See [Downloads and package differences](#downloads-and-package-differences) for the full and minimal builds and their format compatibility.

### Features

- **Models**: PMD and PMX models with bone, IK, morph, and toon rendering support.
- **Animation**: VMD motion and VPD poses, multitrack keyframe editing, playback with physics preview, and project frame-rate correction, plus VSQ and Kinect input.
- **Curve editor**: Edit nine bone channels, morphs, camera, lighting, self-shadow, gravity, and accessories. Adjust handles, insert keys along a curve, and seek with the main timeline. [Curve editing](docs/guides/animation-curves.md).
- **Bone scaling**: Uniform and per-axis bone scaling for local, global, and view coordinates. Scale data requires MMS extensions; original MMD does not apply it. [Bone scaling](docs/guides/bone-scaling.md).
- **Editing capacity**: Keyframes and display lists grow as needed, within memory and save-format limits. Full Unicode names are preserved in MMS extensions. PMM projects beyond original MMD's limits cannot be opened there. [File compatibility](#pmm-and-vmd-files).
- **Physics**: Rigid-body simulation, constraints, and baking to bone motion. Supports frame ranges, presets, undo, and cancellation. [Physics baking](docs/guides/physics-baking.md).
- **Mesh baking and export**: Export captured MME vertex deformation to Alembic or bake it into the project. Clear a bake to return to live deformation. [Mesh export and baking](docs/guides/mesh-export.md).
- **Rendering**: MME effects, GPU skinning, and optional DX12 preview, with DX9 and CPU paths for unsupported hardware. Rendering and vertex processing can use separate devices.
- **Anti-aliasing**: Choose original MSAA, FXAA, TAA, or SMAA to balance smoothing and preview performance. Includes outline softening and AA for still images and image sequences. [Display settings](docs/guides/rendering.md).
- **Viewport and shadows**: Adjust preview resolution without changing export size. Dynamic shadow precision, a wider self-shadow range, optional filtering, and transparency sorting provide scene-specific controls. [Display settings](docs/guides/rendering.md).
- **Media**: Audio and video import, AVI and MP4/H.264 export, transparent MOV, and image sequences. PNG and DDS exports choose transparent backgrounds automatically. Available media formats depend on Windows components.
- **Saving and backups**: Background project saving, scheduled backups, and optional `.pmm1` version retention. Fatal errors save a report and attempt recovery. Error logs use the configured backup folder, which defaults to `backups` beside the EXE.
- **UI**: English, Chinese, and Japanese UI, Unicode file paths, recent projects, undo and redo, and shortcuts that work with an IME outside text fields.

### Building from Source

Dependencies: Windows 10/11, Visual Studio 2022 (v143 toolset + Desktop
development with C++ workload), [CMake](https://cmake.org/) ≥ 3.21,
[Conan](https://conan.io/) 2.x.

```bat
:: 1) Get the Bullet 2.75 sources (used by the local Conan recipe)
::    Download https://github.com/bulletphysics/bullet3/archive/refs/tags/2.75.zip
::    and extract to recipes/bullet275/bullet-src/

:: 2) Register the local recipe and install dependencies (x64 reference)
conan export recipes/bullet275
conan install . --profile:all profiles/x64 --build=missing -s build_type=Release

:: 3) Configure and build
cmake --preset conan-default
cmake --build build --config Release
```

Machines that cannot run D3D12 can keep the DX9/MME renderer and CPU skinning
while omitting the optional DX12 runtime dependency. Configure a separate build:

```bat
cmake -S . -B build/dx12-disabled -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=D:/MMD-Next/MMS/build/generators/conan_toolchain.cmake ^
  -DMIKUMIKUSTUDIO_ENABLE_DX12=OFF
cmake --build build/dx12-disabled --config Release --target MikuMikuStudio
```

This build hides DX12 compute adapters, keeps vertex processing on the CPU,
and skips native DX12 preview. The normal build keeps DX12 enabled. See
[DX12 compatibility build details](docs/records/performance/devices.md#record-dx12-optional-compat).

The build output is `build/Release/`, with
`MikuMikuDance.exe` as its GUI application. After moving the checkout, rerun
`conan install` before using CMake presets to regenerate absolute build paths. The
DirectShow push source used for AVI is linked into the executable, and MP4
recording uses Windows Media Foundation; neither recording path requires an
external `MMDxShow.dll`. Other build products stay out of the runtime directory
(static/import libraries in `build/lib`, PDBs in `build/symbols`). Default Toon
textures are embedded in the executable, so no extra Toon files are required.
To change the default rendering style, place any of `toon01.bmp` through
`toon10.bmp` in the `Data/` directory next to `MikuMikuDance.exe`, then restart
MMS. Each readable file overrides its corresponding embedded texture.
Missing or unreadable files fall back to the embedded defaults. Models with
their own Toon textures continue to use those textures. A release tree
contains the shipped files only:

```bat
cmake --install build --config Release --prefix build/Release
:: build\Release\MikuMikuDance.exe
```

The source repository does not contain compiled release packages. Archives
attached to GitHub Releases are binary distributions, not source files.
`res/assets/` contains application resources; `docs/images/` contains images
used by the documentation.

Regression tests are never part of a release artifact (excluded from the default
build, no install rule). To run them:

```bat
cmake --preset conan-default -DMIKUMIKUSTUDIO_BUILD_INPUT_TESTS=ON
cmake --build build --config Release
cmake --build build --config Release --target keyboard_input_tests
ctest --test-dir build -C Release --output-on-failure
```

> Note: the Conan profiles under `profiles/` assume a Windows + MSVC host; if
> your default profile is already set up that way, you can simply
> `include(default)`.

### MikuMikuEffect (MME)

Place the x64 MME files `d3d9.dll`, `MMHack.dll`, and `MMEffect.dll` beside `MikuMikuDance.exe`, keeping that executable name. The `MMEffect` menu then provides effect assignment. MME is a separate download; supported setups and troubleshooting are in [MME compatibility](docs/guides/effects.md).

### Contributions

This project modifies code that [XiaHan](https://blog.han.moe)
reverse-engineered from the original 32-bit mmd932 and refactored into the
[MikuDanceStudio](https://github.com/jstzwj/MikuDanceStudio) port, which carried
out the bulk of the behavior-level port and the x64 alignment. 洛琪
(GitHub: [Loki-0228](https://github.com/Loki-0228)) maintains this repository
and contributes MME compatibility work, Chinese UI improvements, playback and
input fixes, crash diagnostics, and build and release organization.

### Acknowledgments

- Yu Higuchi (樋口優), original author of
  [MikuMikuDance](https://learnmmd.com/downloads/), whose application provides
  the behavior and feature reference for this project.
- [Bullet Physics](https://github.com/bulletphysics/bullet3) developers and
  contributors, for the physics engine used in this project (zlib license).
- [XiaHan](https://blog.han.moe), who reverse-engineered the original 32-bit
  mmd932 and refactored it into the code this project modifies, and the other
  contributors to the
  [MikuDanceStudio port](https://github.com/jstzwj/MikuDanceStudio).
- 舞力介入P, original author of MikuMikuEffect (MME), for bringing effect
  extensions to MMD. See the [MME introduction coauthored by its creator](https://codezine.jp/article/detail/5997).
- MME community effect authors and contributors, for sharing effects,
  tools, and knowledge.
- 老面星星 (StarNoodle495) for continued testing and suggestions ([BowlRoll](https://bowlroll.net/user/182602)); 沙拉酱 ([Bilibili](https://space.bilibili.com/3493080791517295)) and AHKちゃん for technical guidance; and 微波叮桃子 ([AplayBox](https://www.aplaybox.com/u/972840773)), 靡苦居士 ([Bilibili](https://space.bilibili.com/8545990)), 幽灵_U0 ([Bilibili](https://space.bilibili.com/454701392)), and 低智商鼻嘎 for testing and bug reports.

### License

This project is licensed under the same freeware terms as MikuMikuDance —
see [LICENSE](LICENSE). Third-party components follow their own licenses:
Bullet Physics (zlib), Alembic and Imath (BSD-3-Clause), MinHook (BSD-2-Clause), DirectX SDK / Windows SDK (Microsoft EULA). Export dependency notices are in [licenses](licenses/README.md).

---

## 中文

MikuMikuStudio（MMS）是用于制作和编辑 MMD 舞蹈动画的 Windows 桌面应用。以下功能说明以当前完整档源码为准，完整包、最小包的范围及格式兼容性见[下载与版本区别](#下载与版本区别)。

### 功能

- **模型**：PMD、PMX 模型载入，支持骨骼、IK、表情和 toon 渲染。
- **动画**：VMD 动作与 VPD 姿态、多轨道关键帧编辑、物理预览及工程帧率修正，也支持 VSQ、Kinect 输入。
- **曲线编辑器**：支持骨骼位移、XYZ 旋转和缩放九通道，以及表情、镜头、灯光、自阴影、重力和附件曲线。可调整手柄、沿曲线插帧，并与主时间轴联动。见[曲线编辑](docs/guides/animation-curves.md)。
- **骨骼缩放**：支持整体及 XYZ 缩放，可选择局部、全局和视图坐标系。缩放依赖 MMS 扩展，原版 MMD 不应用这些数据。见[骨骼缩放](docs/guides/bone-scaling.md)。
- **编辑容量**：关键帧和显示项按需扩容，完整 Unicode 名称通过 MMS 扩展保留。扩容仍受内存和保存格式限制，超出原版范围的 .pmm 工程不能在原版 MMD 打开。见[格式兼容说明](#pmm-与-vmd-的区别和兼容性)。
- **物理**：支持刚体与约束模拟，可将结果烧录为骨骼动作，支持帧范围、预设、撤销和取消。见[物理烧录](docs/guides/physics-baking.md)。
- **网格烧录与导出**：可将捕获到的 MME 顶点变形导出为 Alembic，或烧录到工程；清除烧录后恢复实时变形。见[网格导出与烧录](docs/guides/mesh-export.md)。
- **渲染**：支持 MME 特效、GPU 蒙皮和可选 DX12 预览，硬件不支持时使用 DX9、CPU 路径；可分别选择主渲染和顶点计算设备。
- **抗锯齿**：原版 MSAA 与 FXAA、TAA、SMAA 可选，便于平衡画面平滑度和预览性能，支持描边柔化及静帧、图片序列抗锯齿。见[画面设置](docs/guides/rendering.md)。
- **视口与阴影**：可调整预览分辨率而不改变导出尺寸，并按场景选择阴影动态精度、扩展自阴影范围、滤波和透明排序。见[画面设置](docs/guides/rendering.md)。
- **多媒体**：支持音视频导入、AVI、MP4/H.264、透明 MOV 和图片序列输出；PNG、DDS 按格式自动导出透明背景。可用音视频格式取决于 Windows 媒体组件。
- **保存与备份**：支持后台保存、定时备份和可选 `.pmm1` 旧版本保留。致命错误会记录报告并尝试备份；错误日志写入设置的备份目录，默认是程序旁的 `backups`。
- **界面**：英中日三语界面、Unicode 路径、最近工程、撤销与重做；在文本框外，开启输入法也可使用字母快捷键。

### 从源码构建

#### 准备环境

- Windows 10/11。
- Visual Studio 2022：安装 **C++ 桌面开发**工作负载和 **v143** 工具集。
- [CMake](https://cmake.org/) ≥ 3.21、[Conan](https://conan.io/) 2.x。

以下命令均在仓库根目录执行，构建目标为 x64 Release。
`profiles/x64` 会通过 `include(default)` 继承 Conan 默认配置，再指定 x64 架构；
请先确保默认 profile 已配置为 Windows + MSVC。

#### 1. 准备 Bullet 源码

下载 [Bullet 2.75 源码压缩包](https://github.com/bulletphysics/bullet3/archive/refs/tags/2.75.zip)，
将其中的源码目录内容解压到 `recipes/bullet275/bullet-src/`。
确认目录结构为 `recipes/bullet275/bullet-src/src/`，不要多套一层压缩包目录。
本地 Conan 配方会使用这些源码构建依赖。

#### 2. 安装依赖并编译

```bat
conan export recipes/bullet275
conan install . --profile:all profiles/x64 --build=missing -s build_type=Release
cmake --preset conan-default
cmake --build build --config Release
```

MMS 构建产物位于 `build/Release/`。移动仓库目录后，先重新运行
`conan install`，再使用 CMake 预设，避免沿用旧目录的绝对路径。构建完成后，主要文件如下：

| 路径                              | 用途                                 |
| --------------------------------- | ------------------------------------ |
| `build/Release/MikuMikuDance.exe` | GUI 主程序                           |
| `build/lib/`                      | 静态库和导入库                       |
| `build/symbols/`                  | 用于调试和崩溃定位的 PDB 符号文件    |

AVI 使用的 DirectShow 推源过滤器已静态链接进主程序；MP4 使用 Windows
Media Foundation。AVI 和 MP4 录制都不要求用户另行放置或注册 `MMDxShow.dll`。

#### 3. 整理发布目录

```bat
cmake --install build --config Release --prefix build/Release
```

正式发布目录为 `build/Release/`，安装命令也使用同一路径：

```text
build/Release/
├── MikuMikuDance.exe
└── Data/
```

默认 Toon 贴图已内置，无需额外放置 Toon 文件。
如需改变默认渲染风格，可将 `toon01.bmp` 至 `toon10.bmp` 中任意所需的文件放入
`MikuMikuDance.exe` 同目录下的 `Data/`，然后重启 MMS。
程序优先读取这些同名文件，分别覆盖对应的内置 Toon；缺失或读取失败时使用内置贴图。
模型自带并指定的 Toon 贴图仍按模型设置加载。
静态库、导入库、PDB 和回归测试程序不随此命令安装。
源码仓库不包含编译后的发行包；GitHub Release 中的压缩包属于二进制分发文件，不是源码。仓库里的 `res/assets/` 是程序使用的资源文件，`docs/images/` 是文档配图。

#### 可选：运行输入回归测试

测试程序不参与默认构建，需启用选项并显式构建测试目标：

```bat
cmake --preset conan-default -DMIKUMIKUSTUDIO_BUILD_INPUT_TESTS=ON
cmake --build build --config Release --target keyboard_input_tests
ctest --test-dir build -C Release -R keyboard_input_regressions --output-on-failure
```

### MikuMikuEffect（MME）支持

将 x64 MME 的 `d3d9.dll`、`MMHack.dll`、`MMEffect.dll` 放到 `MikuMikuDance.exe` 同目录，并保留主程序名称。启动后通过 `MMEffect` 菜单分配特效。MME 需另行下载，已验证范围及排错方法见 [MME 使用与兼容说明](docs/guides/effects.md)。

### 项目来源与维护

本项目在 [XiaHan](https://blog.han.moe) 对 mmd932 32 位原版逆向并重构的代码基础上修改。
上游的 [MikuDanceStudio](https://github.com/jstzwj/MikuDanceStudio)
移植工程完成了主体行为级移植与 x64 对齐，是本项目继续开发的基础。

本项目由洛琪（[Loki-0228](https://github.com/Loki-0228)）维护，
主要改动包括 MME 兼容适配、中文界面改进、播放与输入修复、崩溃诊断，以及构建与发布整理。

### 致谢

- 樋口優：[MikuMikuDance](https://learnmmd.com/downloads/) 原作者。
  原版 MMD 是本项目行为与功能对齐的基础。
- [Bullet Physics](https://github.com/bulletphysics/bullet3) 开发者与贡献者：
  本项目使用其物理引擎（zlib 许可）。
- [XiaHan](https://blog.han.moe)：本项目在他对 mmd932 32 位原版逆向并重构的代码基础上修改，
  感谢他提供逆向成果与重构代码。也感谢
  [MikuDanceStudio](https://github.com/jstzwj/MikuDanceStudio) 移植工程的其他贡献者。
- 舞力介入P：MikuMikuEffect（MME）原作者，感谢其为 MMD 带来的特效扩展能力。
  可参阅[作者参与撰写的 MME 介绍](https://codezine.jp/article/detail/5997)。
- MME 社区的特效作者与贡献者：感谢他们创作和分享特效、工具与使用经验。
- 老面星星（StarNoodle495，[BowlRoll](https://bowlroll.net/user/182602)）：感谢一直以来持续进行测试并提出建议；感谢沙拉酱（[Bilibili](https://space.bilibili.com/3493080791517295)）和 AHKちゃん 提供技术指导；感谢微波叮桃子（[AplayBox](https://www.aplaybox.com/u/972840773)）、靡苦居士（[Bilibili](https://space.bilibili.com/8545990)）、幽灵_U0（[Bilibili](https://space.bilibili.com/454701392)）和低智商鼻嘎参与测试并提交 bug。

### 许可

本工程采用与 MikuMikuDance 相同的免费软件条款，详见 [LICENSE](LICENSE)。
第三方组件遵循各自许可：Bullet Physics（zlib）、Alembic 与 Imath（BSD-3-Clause）、MinHook（BSD-2-Clause）、DirectX SDK / Windows SDK（微软 EULA）。新增导出依赖的许可文本见 [licenses](licenses/README.md)。

---

## 日本語

MikuMikuStudio（MMS）は、MMD のダンスアニメーションを制作・編集する Windows デスクトップアプリです。以下は現在の完全版ソースの説明です。完全版・最小版の範囲と形式の互換性は[ダウンロードとパッケージの違い](#ダウンロードとパッケージの違い)を参照してください。

### 機能

- **モデル**：PMD・PMX モデルの読み込み、ボーン、IK、モーフ、toon 描画に対応します。
- **アニメーション**：VMD モーションと VPD ポーズ、複数トラックのキー編集、物理プレビュー、プロジェクトのフレームレート修正、VSQ・Kinect 入力に対応します。
- **曲線エディタ**：ボーンの位置・XYZ 回転・スケールの 9 チャンネルに加え、モーフ、カメラ、照明、セルフシャドウ、重力、アクセサリを曲線で編集できます。ハンドル編集、曲線上へのキー挿入、メインタイムラインとの同期に対応します。[曲線編集](docs/guides/animation-curves.md)。
- **ボーンスケール**：ボーンを一括または XYZ 軸ごとに拡大縮小でき、ローカル・グローバル・ビュー座標を選べます。スケールは MMS 拡張で保存し、オリジナル MMD には適用されません。[ボーンスケール](docs/guides/bone-scaling.md)。
- **編集容量**：メモリと保存形式の範囲内でキーフレームと表示項目の容量を拡張し、完全な Unicode 名を MMS 拡張に保持します。従来の制限を超えた PMM はオリジナル MMD で開けません。[形式の互換性](#pmm-と-vmd-の違いと互換性)。
- **物理**：剛体と拘束のシミュレーションに対応し、結果をボーンモーションにベイクできます。範囲、プリセット、元に戻す操作、キャンセルに対応します。[物理ベイク](docs/guides/physics-baking.md)。
- **メッシュベイクと出力**：取得した MME の頂点変形を Alembic に出力、またはプロジェクトにベイクできます。ベイクを消すとリアルタイム変形に戻ります。[メッシュ出力とベイク](docs/guides/mesh-export.md)。
- **描画**：MME、GPU スキニング、任意の DX12 プレビューに対応します。非対応環境では DX9・CPU を使い、描画と頂点処理のデバイスを別々に選べます。
- **アンチエイリアス**：従来の MSAA と FXAA・TAA・SMAA を、滑らかさとプレビュー負荷に応じて選べます。輪郭線の平滑化と、静止画・連番画像の AA にも対応します。[画面設定](docs/guides/rendering.md)。
- **ビューポートと影**：出力サイズを変えずにプレビュー解像度を調整できます。影の動的精度、セルフシャドウ範囲の拡張、フィルター、透明ソートをシーンに合わせて選べます。[画面設定](docs/guides/rendering.md)。
- **メディア**：音声・動画の読み込み、AVI・MP4/H.264・透明 MOV・連番画像の出力に対応します。PNG・DDS は形式に応じて透明背景を使います。利用可能なメディア形式は Windows の構成に依存します。
- **保存とバックアップ**：バックグラウンド保存、定期バックアップ、任意の `.pmm1` 履歴保存に対応します。致命的エラーでは報告を記録してバックアップを試みます。ログの保存先は指定したバックアップフォルダーで、初期設定は EXE 横の `backups` です。
- **UI**：英語・中国語・日本語 UI、Unicode パス、最近使ったプロジェクト、元に戻す・やり直す操作に対応します。文字入力欄以外では IME 使用中も英字ショートカットを使えます。

### ソースからのビルド

必要環境：Windows 10/11、Visual Studio 2022（v143 ツールセット＋「C++ による
デスクトップ開発」ワークロード）、[CMake](https://cmake.org/) ≥ 3.21、
[Conan](https://conan.io/) 2.x。

```bat
:: 1) Bullet 2.75 のソースを入手（ローカル Conan レシピで使用）
::    https://github.com/bulletphysics/bullet3/archive/refs/tags/2.75.zip をダウンロードし、
::    recipes/bullet275/bullet-src/ に展開

:: 2) ローカルレシピを登録して依存関係をインストール（x64 版に対応）
conan export recipes/bullet275
conan install . --profile:all profiles/x64 --build=missing -s build_type=Release

:: 3) 構成してビルド
cmake --preset conan-default
cmake --build build --config Release
```

成果物は `build/Release/MikuMikuDance.exe`（GUI アプリケーション）です。AVI 用の
DirectShow プッシュソースは実行ファイルに静的リンクされ、MP4 録画は Windows Media
Foundation を使用します。いずれも外部の `MMDxShow.dll` を配置・登録する必要はありません。
静的／インポートライブラリ（`build/lib`）と PDB（`build/symbols`）は実行ディレクトリに入りません。
既定のトゥーンテクスチャは実行ファイルに内蔵されているため、追加の配置は不要です。
既定の描画スタイルを変更する場合は、`toon01.bmp` から `toon10.bmp` のうち必要なファイルを
`MikuMikuDance.exe` と同じディレクトリの `Data/` に配置し、MMS を再起動してください。
読み込める同名ファイルが対応する内蔵テクスチャより優先されます。
ファイルがない場合や読み込みに失敗した場合は、内蔵テクスチャを使用します。
モデル独自のトゥーンテクスチャは、引き続きモデルの設定に従って読み込まれます。
テスト実行ファイルは `EXCLUDE_FROM_ALL` のため既定のビルドにも配布物にも含まれません。
配布ディレクトリには配布ファイルのみが含まれます：

```bat
cmake --install build --config Release --prefix build/Release
:: build\Release\MikuMikuDance.exe
```
ソースリポジトリにはビルド済みの配布パッケージを含めません。GitHub Releases に添付する
アーカイブはソースではなくバイナリ配布物です。`res/assets/` はアプリケーションのリソース、
`docs/images/` はドキュメント用の画像です。

> 注：`profiles/` 配下の Conan プロファイルはホストが Windows + MSVC であることを
> 前提としています。デフォルトプロファイルが既にその設定であれば、そのまま
> `include(default)` できます。

### MikuMikuEffect（MME）対応

x64 MME の `d3d9.dll`・`MMHack.dll`・`MMEffect.dll` を `MikuMikuDance.exe` と同じ場所に置き、実行ファイル名を維持してください。起動後は `MMEffect` メニューからエフェクトを割り当てます。MME は別途入手してください。検証範囲と対処方法は [MME の使い方と互換性](docs/guides/effects.md)を参照してください。

### 貢献

本プロジェクトは、[XiaHan](https://blog.han.moe) 氏が mmd932 32 ビット版オリジナルを
リバースエンジニアリングしてリファクタリングしたコードを基に改変したものです。
移植本体と x64 対応は同氏によるもので、上流の
[MikuDanceStudio](https://github.com/jstzwj/MikuDanceStudio) 移植プロジェクトとして
公開されています。洛琪（GitHub：[Loki-0228](https://github.com/Loki-0228)）が
本リポジトリを保守し、MME 互換対応、中国語 UI の改善、再生・入力の不具合修正、
クラッシュ診断、ビルド・配布構成の整備に取り組んでいます。

### 謝辞

- 樋口優氏：[MikuMikuDance](https://learnmmd.com/downloads/) の原作者。
  オリジナル MMD の動作と機能を本プロジェクトの目標としています。
- [Bullet Physics](https://github.com/bulletphysics/bullet3) の開発者・貢献者の皆様：
  本プロジェクトで使用する物理エンジン（zlib ライセンス）に感謝します。
- [XiaHan](https://blog.han.moe) 氏：本プロジェクトは、同氏が mmd932 32 ビット版
  オリジナルをリバースエンジニアリングしリファクタリングしたコードを基に改変した
  ものです。同氏ならびに上流の
  [MikuDanceStudio](https://github.com/jstzwj/MikuDanceStudio) の貢献者の皆様に
  感謝します。
- 舞力介入P 氏：MikuMikuEffect（MME）の原作者。
  MMD にエフェクト拡張機能をもたらしてくださったことに感謝します。
  [作者が共著した MME 紹介記事](https://codezine.jp/article/detail/5997)もご覧ください。
- MME コミュニティのエフェクト作者・貢献者の皆様：
  エフェクト、ツール、ノウハウの共有に感謝します。
- 老面星星氏（StarNoodle495、[BowlRoll](https://bowlroll.net/user/182602)）：継続的なテストと提案に感謝します。沙拉酱氏（[Bilibili](https://space.bilibili.com/3493080791517295)）と AHKちゃん には技術指導を、微波叮桃子氏（[AplayBox](https://www.aplaybox.com/u/972840773)）、靡苦居士氏（[Bilibili](https://space.bilibili.com/8545990)）、幽灵_U0 氏（[Bilibili](https://space.bilibili.com/454701392)）、低智商鼻嘎氏にはテストとバグ報告に感謝します。

### ライセンス

本プロジェクトは MikuMikuDance と同じフリーウェア条項の下で配布されます。
詳細は [LICENSE](LICENSE) を参照してください。サードパーティコンポーネントは
それぞれのライセンスに従います：Bullet Physics（zlib）、
DirectX SDK / Windows SDK（マイクロソフト EULA）。
