# MikuMikuStudio (MMS)

[English](#english) | [中文](#中文) | [日本語](#日本語)

[Release notes / 发布日志 / リリースノート](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.19.1)

[Documentation / 文档导航 / ドキュメント](docs/README.md)

[v2026.10.19.1 Release / 正式版 / 正式版](docs/records/releases.md#release-v2026-10-19-1)

## Downloads and package differences

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.19.1) provides Windows x64 packages with `full` or `minimal` in the filename and lists their contents in the release notes. The table below describes the released build profiles. The minimal build uses the `mmd932` build profile:

| Area | Full `full` | Minimal `minimal` |
| --- | --- | --- |
| PMD and PMX loading, original animation tools, rigid-body physics, language fixes, MME interface | Included | Included |
| Image sequences, periodic backups, crash backup attempts, and morph force paste | Included | Included |
| Equivalent skinning and morph optimizations, unchanged-model reuse, and camera-only outline updates with CPU skinning | Included | Included |
| MME auto-reload using file-change notifications | Included | Included |
| Preview shadow resolution | Scales the built-in renderer's shadow map; MME-owned targets follow their FX declarations | No preview scaling |
| Recent projects, timeline force-paste button, and viewport render scaling | Included | Omitted |
| FXAA, TAA, SMAA | Included; NVIDIA fix DLL is a separate optional download | Original anti-aliasing switch only |
| New transparency sorting | Optional, disabled by default | Omitted; original single-pass material order |
| Shadow reconstruction, dynamic precision, filtering | Clipping prevention and dynamic resolution; optional shared shadow-map filtering, disabled by default | Original projection and SM2/SM3 shaders; no enhanced-filter control |
| Bone scaling, nine-channel bone curves, and morph curves | Included | Omitted; original Bezier interpolation retained |
| DX12, mesh capture, Alembic, additional baking | Included | Omitted |
| Media | Modern imports and MP4/MOV export through Windows media components | WAV audio, AVI backgrounds and AVI export |
| PMM, VMD, VPD saving | Can include MMS extensions | Standard data only; imported MMS extensions are ignored |

Both application ZIPs contain `MikuMikuDance.exe` and licenses. Neither includes models, user settings, MME, or the NVIDIA fix DLL. Starting with `v2026.10.15.1`, `MMSNvidiaAAFix.dll` is a separate optional release asset with [installation instructions in the release notes](docs/records/releases.md#optional-nvidia-fix).

The minimal build checks names and file references during import and rejects text that cannot fit the standard CP932 fields. Compatible PMX models remain available for scenes and animation. Model editing supports PMD only and rejects PMX before opening an editor. Save-time checks remain as a safeguard for later edits. Save extended full-build projects as separate copies when using the minimal build, because scale, curve, and mesh-cache extensions are discarded. Round-trip loading and saving with the actual MMD 9.32 executable has not yet been validated. See [minimal build limits](docs/reference/minimal-build.md).

Extract the two builds into separate directories and run the executable in each directory. Keep your existing installation if it contains settings or plugins.

### PMM and VMD files

PMM saves a project, including model and media references, scene settings, and keyframes. VMD saves motion tracks for use with another model or project; it does not replace a PMM project file.

PMM `0002` extensions preserve bone curves, project settings, and scene and accessory curve records without changing the standard file header. VMD tail extensions store supported curves and bone-scale data while retaining the standard VMD sections. Re-saving an extended project in original MMD may discard MMS-only data. See [curve editor](docs/guides/animation-curves.md) and [PMM v2 extensions](docs/reference/pmm-extensions.md).

The full build also provides the following extended-save behavior.

PMM saving prefers the original `0002` header. Unused reserved space does not force a new format, and sparse high key slots are compacted when their live contents still fit. Only content outside the original limits or legacy path encoding selects the MMS `0003` format. Original MMD and the minimal build cannot open that format.

If names cannot be encoded without loss, MMS preserves the complete names in an extension and reports the difference. It also writes a `.compat.pmm` or `.compat.vmd` copy with legacy short names when that copy is usable. Existing copies receive a numbered suffix. Capacity overflow, ambiguous short names, or unsupported reference paths prevent a compatible copy; MMS reports this instead of dropping keys or assigning motion to the wrong bone.

Processor choices are saved per Windows user and take effect on the next
startup. They are not stored in project files.

## 下载与版本区别

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.19.1) 同时提供 Windows x64 完整包和最小包，实际收录内容见发布页面的日志。需要 MMS 新增功能时选择文件名含 `full` 的 ZIP；主要使用 MMD 原有功能时选择含 `minimal` 的 ZIP。下表描述已发布的两种构建档，两档均保留 `MikuMikuDance.exe` 文件名。

| 项目 | 完整包 `full` | 最小包 `minimal`（构建档 `mmd932`） |
| --- | --- | --- |
| PMD、PMX 载入，原有骨骼、表情、相机、灯光和刚体物理 | 保留 | 保留 |
| 原生功能、输入、编码及 MME 兼容修复 | 保留 | 保留 |
| 图片序列、周期备份、崩溃备份尝试、表情强制粘贴 | 保留 | 保留 |
| 蒙皮与表情的等价计算优化、未变化模型缓存、CPU 蒙皮下相机移动时仅更新描边 | 保留 | 保留 |
| MME 自动重载使用文件变更通知 | 保留 | 保留 |
| 预览阴影分辨率 | 只调整默认渲染的阴影贴图；MME 自建目标遵循 FX 声明 | 不包含预览缩放 |
| 最近工程、时间轴强制粘贴按钮、视口渲染比例 | 保留 | 不包含 |
| FXAA、TAA、SMAA | 保留；NVIDIA 补丁 DLL 另行按需下载 | 仅保留原抗锯齿开关 |
| 新透明排序 | 可选低、标准、高、最高；默认关闭 | 不编入排序与剥层实现，使用原始单遍材质顺序 |
| 阴影重构、动态精度和滤波 | 保留防裁切与动态精度；可选「滤波加强」，默认关闭 | 使用原版投影及 SM2/SM3 着色器，无「滤波加强」 |
| 骨骼缩放、九通道骨骼曲线和表情曲线 | 保留 | 不编入，保留原有 Bezier 插值 |
| DX12、通用 mesh 捕获、Alembic 和新增烘焙 | 保留 | 不编入对应实现 |
| 音视频 | 保留现代音视频导入与 MP4/MOV 输出，依赖 Windows 媒体组件 | WAV 音频、AVI 背景与 AVI 输出 |
| PMM、VMD、VPD 保存 | 可写 MMS 扩展数据 | 仅写标准内容，忽略读取到的 MMS 扩展 |
| MME | 保留原生 DX9 接口，插件需另行安装 | 保留原生 DX9 接口，插件需另行安装 |

完整包和最小包 ZIP 均包含 `MikuMikuDance.exe` 与许可文件，不附带用户配置、模型、MME 或 NVIDIA 修复 DLL。从 `v2026.10.15.1` 起，`MMSNvidiaAAFix.dll` 作为独立附件按需下载，见[Release Notes 中的安装说明](docs/records/releases.md#nvidia-修复安装可选)。

最小包仍保留三语界面等便利修复，并非原版 MMD 的逐字节复刻。它在导入时检查名称和文件内引用：须能完整表示为 CP932，并符合标准字段的字节上限，超出范围时拒绝导入。合规 PMX 仍可用于场景和动画；模型编辑仅支持 PMD，PMX 在进入模型编辑时即被拦截。保存端保留校验，防止后续编辑产生无法表示的内容。

含缩放、曲线或 mesh 缓存等扩展的完整包工程，经最小包保存后会丢失这些扩展，请另存为副本。真实 MMD 9.32 程序的双向载入及再保存尚未完成验证，不能据此保证任意工程无损互导。选择下载前可查看[最小包范围与格式限制](docs/reference/minimal-build.md)及[透明排序验收记录](docs/records/rendering/transparency.md#record-transparent-sort-validation)。

两档解压到不同目录，分别运行各自的 `MikuMikuDance.exe`。有旧版配置和插件时，先保留旧目录；不要把两个 ZIP 解压到同一目录后相互覆盖。

### PMM 与 VMD 的区别和兼容性

PMM 保存完整工程，包括模型与媒体引用、场景设置和关键帧。VMD 保存动作轨道，便于应用到其他模型或工程，不能代替 PMM 工程文件。

PMM `0002` 在保留标准文件头的前提下，通过扩展保存骨骼曲线、工程设置及场景、附件曲线记录。支持的曲线和骨骼缩放也可写入 VMD 尾部扩展，标准数据段保持不变。原版 MMD 重新保存扩展工程时可能丢弃 MMS 专有数据。详见[曲线编辑器](docs/guides/animation-curves.md)和[PMM v2 扩展](docs/reference/pmm-extensions.md)。

完整包还支持以下扩展保存行为。

保存 PMM 时优先采用旧版 `0002`。未使用的预留空间不会触发新版格式；高位关键帧槽位存在空洞时，保存器会压紧槽位。只有实际内容超出旧版能力或引用路径无法用旧编码表示时，才采用 MMS `0003`。原版 MMD 和最小包不能打开该格式。

名称无法无损编码时，MMS 在扩展中保留完整名称并提示，同时在能生成可用副本时自动写入 `.compat.pmm` 或 `.compat.vmd`。副本采用旧格式短名称，同名副本添加序号。容量不足、短名称冲突或引用路径不兼容时，MMS 会说明无法生成兼容副本，不会删减关键帧或把动作分配给错误的骨骼。

运算器选择按 Windows 用户跨工程保存，下次启动生效，不写入工程文件。

## ダウンロードとパッケージの違い

[GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases/tag/v2026.10.19.1) では、ファイル名に `full` または `minimal` を含む Windows x64 パッケージを配布しています。収録内容は公開ページのリリースノートを参照してください。下表は公開済みのビルド構成です。最小版は `mmd932` ビルドプロファイルを使用します。

| 項目 | 完全版 `full` | 最小版 `minimal` |
| --- | --- | --- |
| PMD・PMX 読み込み、従来のアニメーション編集、剛体物理、言語関連の修正、MME インターフェース | 含む | 含む |
| 連番画像、定期バックアップ、クラッシュ時のバックアップ試行、モーフの強制貼り付け | 含む | 含む |
| スキニング・モーフの等価な計算最適化、変化のないモデルの再利用、CPU スキニングでのカメラ移動時の輪郭のみの更新 | 含む | 含む |
| ファイル変更通知による MME 自動再読み込み | 含む | 含む |
| プレビューのシャドウ解像度 | 標準レンダラーのシャドウマップのみ調整。MME 独自のターゲットは FX の宣言に従う | プレビュー縮小なし |
| 最近使ったプロジェクト、タイムラインの強制貼り付けボタン、ビューポート描画倍率 | 含む | 含まない |
| FXAA、TAA、SMAA | 含む。NVIDIA 修正 DLL は任意の別ダウンロード | 従来のアンチエイリアス切り替えのみ |
| 新しい透明ソート | 任意で有効化、初期設定は無効 | 含まない。従来の単一パスの材質順序を使用 |
| シャドウ処理の再構成、動的精度、フィルタリング | クリッピング防止、動的精度、共有シャドウマップのフィルター強化（初期設定は無効） | 従来の投影と SM2/SM3 シェーダー。平滑化スイッチなし |
| ボーンスケール、9 チャンネルのボーン曲線、モーフ曲線 | 含む | 含まない。従来の Bezier 補間を維持 |
| DX12、メッシュ取得、Alembic、追加のベイク機能 | 含む | 含まない |
| 音声・動画 | Windows メディアコンポーネントによる各種形式の読み込みと MP4・MOV 出力 | WAV 音声、AVI 背景、AVI 出力 |
| PMM・VMD・VPD 保存 | MMS 拡張を保存可能 | 標準データのみ保存。読み込んだ MMS 拡張は無視 |

両方のアプリ ZIP に `MikuMikuDance.exe` とライセンスを含みます。モデル、ユーザー設定、MME、NVIDIA 修正 DLL は同梱しません。`v2026.10.15.1` 以降、`MMSNvidiaAAFix.dll` は任意の独立ダウンロードです。[リリースノートの導入手順](docs/records/releases.md#nvidia-修正の導入任意)を参照してください。

最小版は読み込み時に名前と参照先の CP932 表現およびフィールド長を確認し、表現できない内容の読み込みを拒否します。条件を満たす PMX はシーンやアニメーションで利用できます。モデル編集は PMD のみ対応し、PMX は編集画面を開く前に拒否します。読み込み後の編集に備え、保存時の確認も残しています。完全版の拡張を含むプロジェクトを最小版で保存すると、スケール、曲線、メッシュキャッシュの拡張が失われるため、別名で保存してください。実際の MMD 9.32 を使った双方向の読み込みと再保存は、まだ検証できていません。[最小版の制限](docs/reference/minimal-build.md)も参照してください。

両方を使う場合は別々のフォルダーに展開し、それぞれの実行ファイルを起動してください。設定やプラグインが入った既存のフォルダーは残してください。

### PMM と VMD の違いと互換性

PMM はモデルとメディアの参照、シーン設定、キーフレームを含むプロジェクトを保存します。VMD は別のモデルやプロジェクトに適用するモーションを保存します。VMD は PMM プロジェクトの代わりにはなりません。

PMM `0002` は標準ヘッダーを維持し、拡張データとしてボーン曲線、プロジェクト設定、シーンとアクセサリの曲線を保存します。対応する曲線とボーンスケールは VMD の末尾拡張にも保存でき、標準データ部分は保持します。オリジナル MMD で再保存すると MMS 固有データが失われることがあります。[曲線エディタ](docs/guides/animation-curves.md)と[PMM v2 拡張](docs/reference/pmm-extensions.md)を参照してください。

完全版には、次の拡張保存機能もあります。

PMM は従来の `0002` 形式を優先します。未使用の予約領域では新形式に切り替わりません。高い番号に残ったキーは、保存時に空きスロットを詰めます。実際の内容が従来の範囲を超える場合、または参照パスを旧文字コードで表現できない場合にだけ、MMS `0003` 形式を使います。オリジナル MMD と最小版はこの形式を開けません。

名前を無損失で保存できない場合は、拡張に完全な名前を保存して通知します。利用可能な互換コピーを作れる場合は、旧形式の短い名前を使った `.compat.pmm` または `.compat.vmd` を自動保存します。同名のコピーは上書きせず連番を付けます。容量不足、短縮名の重複、参照パスの制約がある場合は、キーの省略や誤ったボーンへの割り当てを行わず、コピーを作れない理由を通知します。

処理デバイスの選択は Windows ユーザーごとに保存され、次回起動時に反映されます。プロジェクトファイルには保存されません。

---

## English

MikuMikuStudio (MMS) is a Windows desktop application for creating and editing MMD dance animations. The following features describe the current full-build source. See [Downloads and package differences](#downloads-and-package-differences) for the full and minimal builds and their format compatibility.

### Features

- **Models**: PMD / PMX loading (incl. BDEF4/SDEF weights), bone trees, IK,
  morphs, toon rendering
- **Animation**: VMD load/save, VPD export, frame editor (bone / morph /
  camera / lighting / self-made expression / accessory tracks), whole-project
  frame-rate correction, and frame playback with physics preview
- **Physics**: Bullet 2.75 rigid bodies + constraints (6DOF spring),
  deterministic solve order, physics baking and per-frame bone-motion
  resampling. Supports frame ranges, ERP/CFM, substeps, simulation rate,
  undo/cancel, and unencrypted UTF-8 JSON presets. The baking UI has English,
  Chinese, and Japanese labels and tooltips, with Bullet defaults. See
  [physics baking](docs/guides/physics-baking.md).
- **Mesh baking and export**: Alembic export includes MME vertex deformations. Exporting ABC and baking into the project can be selected
  independently; project baking warns about its effects. Clear mesh bake
  restores live deformation for the current model or all objects while keeping
  disabled FX disabled. See [mesh export and baking](docs/guides/mesh-export.md).
- **Rendering**: Native DX12 preview and GPU skinning with DX9/CPU fallback when
  DX12 is unavailable; MME Effect support, toon textures, ground shadows,
  stereoscopic 3D (NVIDIA 3D Vision), and separate device choices for main
  rendering and vertex processing
- **Multimedia**: Imports many audio formats and MP4/MOV video through Windows
  Media Foundation, depending on installed Windows media components;
  plays Wave audio; records AVI through installed DirectShow codecs, MP4/H.264
  through Media Foundation, and transparent MOV. The export dialog lists
  registered H.264 encoders and supports aspect-ratio-preserving output sizes.
  VSQ and Kinect input are also supported.
- **UI and compatibility**: English/Chinese/Japanese interface, Unicode file
  paths, timeline, accessory editing, undo/redo

#### Automatic editing capacity

Names retain their complete Unicode text. Bone, morph, display, camera, light, shadow, gravity, and accessory key pools grow as needed. Display groups and facial display lists also retain counts beyond the original byte fields. No capacity settings are required.

Frame numbers use checked arithmetic through 2,147,483,646, with precise playback time for adjacent keys at large frames. Memory and file field sizes still bound automatic growth. For saving and legacy compatibility, see [PMM and VMD files](#pmm-and-vmd-files).

#### Image sequence export and backups

Both packages support image sequence export with a directory, filename prefix, image format, and frame range. The format choices match single-image export, and numbering width follows the output range.

Auto-save settings control the interval in minutes, backup directory, maximum count, and replacement of older backups. The default is `backups` beside the executable, without overwriting manually saved projects. Crash handling also attempts a backup; completion depends on the crash state and destination access.

#### Viewport and shadow controls

The full build can match the viewport to the window, render a lower-resolution preview, or use supersampling without changing export dimensions. Lower-resolution previews use nearest-neighbor upscaling. The HUD and toolbars render at display resolution across the full preview area, including letterbox bars. Numeric input fields shorten before text shrinks. When a toolbar does not fit on one row, it uses two rows if the adjacent letterbox bar has enough room. FPS text and HUD controls keep their normal size, shrinking only when the preview area cannot fit them. FPS counts completed, successfully submitted scene frames.

Self-shadowing restores MMD 9.32 projection and depth sampling. Dynamic resolution and fixed resolution share the same shadow-range setting. Optional stronger filtering, disabled by default, applies two B-spline passes to the shadow map before native materials and MME receive it. A failed filter falls back to the current frame's original map.

#### Anti-aliasing and project settings

The full build offers original MMD anti-aliasing (MSAA), FXAA, motion-vector TAA,
and SMAA. In the current source, **SMAA** only opens its submenu; it has no default choice.
Choose 1x or T2x in that submenu, with standard edges or diagonal and corner enhancement.
Selection dots appear beside SMAA and its current variant only while SMAA is selected.
T2x combines two jittered frames using motion vectors.
TAA and T2x keep current and previous deformed positions in GPU textures when
the adapter supports vertex texture sampling. They share captures across
matching material inputs and reuse the original index buffers. Verified opaque
native materials skip the extra coverage draw. MME shaders and alpha textures
keep their coverage checks. Capability checks apply to NVIDIA, AMD, and Intel;
devices without vertex texture sampling retain CPU geometry capture.
Complex MME effects still add work. SMAA 1x avoids motion capture, and unsupported
motion passes use spatial AA for that frame.

Temporal image export fills missing samples at the same scene time, including
projects containing only frame zero. A continuous image sequence retains its
history: T2x needs two scene renders for the first image and one for each next
image. Sampling does not advance animation or physics and does not add files.
PMM v2 extensions store the AA switch, algorithm, and last SMAA choice. Loading
a project does not overwrite local defaults. See [PMM settings](docs/reference/pmm-extensions.md).

Download the optional `MMSNvidiaAAFix.dll` separately if you need the fix. Place it beside the full build's `MikuMikuDance.exe` to manage the fix from **Help**. Copying the DLL does not install the driver setting. When the menu opens, the DLL checks this program's setting and offers **Install** or **Uninstall**. Either operation requests Windows administrator approval and takes effect the next time MMS starts.

The fix applies to NVIDIA only. Installation stops if no usable NVIDIA driver is available; AMD and Intel do not receive this setting. Uninstalling warns that MSAA may show white edges or seams again and removes only this program's setting. Deleting the DLL hides the menu entry without undoing an installed setting.

### Curve editor

The native curve editor edits nine bone channels for position, XYZ rotation, and scale, plus morph, camera, lighting, self-shadow, gravity, and accessory tracks. It supports key insertion that preserves the curve shape, numeric control points, slope and weight dragging, and keyframe navigation.

The source and history audit is in [MMS native feature audit](docs/records/development/features.md#record-mms-native-features-audit).

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
  -DMIKUDANCESTUDIO_ENABLE_DX12=OFF
cmake --build build/dx12-disabled --config Release --target MikuDanceStudio
```

This build hides DX12 compute adapters, keeps vertex processing on the CPU,
and skips native DX12 preview. The normal build keeps DX12 enabled. See
[DX12 compatibility build details](docs/records/performance/devices.md#record-dx12-optional-compat).

The final MMS package is `D:/MMD-Next/MMS/build/Release/`, with
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
cmake --preset conan-default -DMIKUDANCESTUDIO_BUILD_INPUT_TESTS=ON
cmake --build build --config Release
cmake --build build --config Release --target keyboard_input_tests
ctest --test-dir build -C Release --output-on-failure
```

> Note: the Conan profiles under `profiles/` assume a Windows + MSVC host; if
> your default profile is already set up that way, you can simply
> `include(default)`.

### MikuMikuEffect (MME)

The executable keeps the original name `MikuMikuDance.exe` and meets MME's three
host requirements: the 37 `Exp*` exports by ordinal 1..37
(`exports/MikuMikuDance.def`); **static** imports of the `d3dx9_43.dll` entries
MMHack.dll patches - it rewrites the _host's_ IAT, so a host that resolved D3DX
at runtime exposed no slot and MME aborted with `Initialize error`
(`res/imports/d3dx9_43.def`, import library generated at build time); and the
exact module name, from which MMHack.dll's import table binds those `Exp*`
functions. Copy the x64 `d3d9.dll`, `MMHack.dll` and `MMEffect.dll` next to it
and the `MMEffect` menu appears; `.fx` effects are then assigned as usual. No
DirectX SDK, at runtime or at build time. See
[MME compatibility](docs/guides/effects.md).

### Personal usage changes

These additions exist only for personal convenience; they are neither MMD's
original usage habits nor compatibility requirements, and builders may delete
them by editing the code.

- **Native bone scaling**: Uniform and XYZ controls support local, global and view axes for every bone. PMM preserves full scale data; VMD stores scale in a versioned tail extension while preserving its standard prefix. See [bone scaling](docs/guides/bone-scaling.md).
- **Extended self-shadow range**: Removes the original MMD restriction on the range where self-shadows can be generated.
- **Dynamic shadow scaling**: Matches shadow-map resolution to the projected density of visible geometry within the shadow-quality and device limits. Dynamic and fixed resolution share one shadow-range slider; there is no separate precision value. The full build preserves shadow coverage at high precision to reduce clipping.
- **Error reporting**: The program writes no log files by default; diagnostic file tracing can be enabled explicitly. A fatal error raises a
  modal window whose text can be copied with Ctrl+C, and the PDB under
  `build/symbols/` resolves source lines.
- **IME and hotkeys**: Letter hotkeys keep working with a Chinese/Japanese IME
  active - P play/stop plus A/S/D/F/G/H/I/J/K/L/U/V/X/Z/B/C/R - except inside
  text fields.
- **Transparent sorting preference**: Adds five selectable sorting levels,
  including disable, to adjust transparency ordering for personal scenes.

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
- 老面星星 (StarNoodle495) for continued testing and suggestions ([BowlRoll](https://bowlroll.net/user/182602)); 沙拉酱 ([Bilibili](https://space.bilibili.com/3493080791517295)) for technical guidance; and 微波叮桃子 ([AplayBox](https://www.aplaybox.com/u/972840773)), 靡苦居士 ([Bilibili](https://space.bilibili.com/8545990)), 幽灵_U0 ([Bilibili](https://space.bilibili.com/454701392)), and 低智商鼻嘎 for testing and bug reports.

### License

This project is licensed under the same freeware terms as MikuMikuDance —
see [LICENSE](LICENSE). Third-party components follow their own licenses:
Bullet Physics (zlib), Alembic and Imath (BSD-3-Clause), MinHook (BSD-2-Clause), DirectX SDK / Windows SDK (Microsoft EULA). Export dependency notices are in [licenses](licenses/README.md).

---

## 中文

MikuMikuStudio（MMS）是用于制作和编辑 MMD 舞蹈动画的 Windows 桌面应用。以下功能说明以当前完整档源码为准，完整包、最小包的范围及格式兼容性见[下载与版本区别](#下载与版本区别)。

### 功能

- **模型**：PMD / PMX（含 BDEF4/SDEF 权重）加载，骨骼树、IK、形态（morph）、toon 渲染
- **动画**：VMD 加载/保存、VPD 导出、帧编辑器（骨骼/形态/相机/照明/自作表情/附件轨道）、
  整个工程的帧率修正、帧回放与物理预览
- **物理**：Bullet 2.75 刚体与约束（6DOF spring），确定性求解顺序；支持烧录物理结果和逐帧重采样骨骼动作。可设置帧范围、ERP、CFM、子步数和计算频率，支持撤销、取消及 UTF-8 非加密 JSON 预设。界面提供中英日三语和悬浮说明，初始值采用 Bullet 默认参数。见[物理烧录](docs/guides/physics-baking.md)
- **网格烧录与导出**：Alembic 可导出包含 MME 顶点变形的网格；导出 ABC 与烧录回工程可分别选择，只有选择烧录时才提示影响。清除网格烧录可恢复当前模型或全部对象的实时变形，已停用的 FX 保持关闭。见[网格导出与烧录](docs/guides/mesh-export.md)
- **渲染**：原生 DX12 预览与 GPU 蒙皮；DX12 不可用时回退到 DX9/CPU。支持 MME Effect、toon 纹理、地面阴影、立体视觉（NVIDIA 3D Vision），并可分别选择主渲染和顶点计算设备
- **多媒体**：通过 Windows Media Foundation 导入多种音频格式和 MP4/MOV 视频，具体格式取决于系统媒体组件。支持 Wave 音频播放、DirectShow AVI（使用已安装的传统编码器）、Media Foundation MP4/H.264 录制，以及透明 MOV 录制。导出页列出系统已注册的 H.264 编码器并支持等比例修改分辨率。另支持 VSQ 乐谱导入和 Kinect 骨骼输入
- **UI 与兼容**：英中日三语界面、Unicode 文件路径、时间轴、附件编辑、撤销/重做

#### 编辑容量自动扩展

名称保留完整 Unicode 文本。骨骼、表情、显示、镜头、灯光、自阴影、重力和附件关键帧按需扩容；显示分组和表情显示项也不再受原有单字节计数限制，无需手动填写容量。

帧号通过溢出检查扩展至 2,147,483,646，大帧号播放仍能区分相邻关键帧。自动扩容受可用内存和文件字段范围约束。保存与旧版兼容规则见 [PMM 与 VMD 的区别和兼容性](#pmm-与-vmd-的区别和兼容性)。

#### 图片序列与自动备份

两个版本均支持图片序列输出，可设置目录、文件名前缀、图片格式和帧范围。格式与单帧图片输出一致，连号位数随输出范围调整。

「自动保存设置…」可配置分钟间隔、备份目录、数量上限和覆盖旧备份。默认写入程序同级的 `backups`，不覆盖手动保存的工程。崩溃处理也会尝试备份，能否完成取决于当时的程序状态及目录可写性。

#### 视口与阴影控制

完整包支持自动匹配窗口、低比例预览和超采样，不改变导出尺寸。低比例画面使用最近邻放大；HUD 和工具条在放大后按显示分辨率绘制，包含黑边区域。数值输入框会优先缩短，尽量保留文字大小。工具条一行排布不下时，若相邻黑边有足够空间，则改为两行。帧率文字和 HUD 保持正常大小，仅在预览区域容纳不下时缩小。FPS 统计实际完成并成功提交的场景帧。

自阴影恢复 MMD 9.32 的投影和深度采样。动态精度与固定精度共用阴影范围设置。「滤波加强」默认关闭，开启后先对阴影图进行两遍基础样条滤波，再交给原生材质和 MME；失败时使用当帧原始阴影图。

#### 抗锯齿与工程设置

完整版本提供 MMD 原始（MSAA）、FXAA、带运动矢量的 TAA 和 SMAA。
当前源码中的「SMAA」只展开子菜单，不设默认档位；只能在三级菜单中选择具体档位。
只有当前选择 SMAA 时，二级「SMAA」和三级当前档位才同时显示圆点。可选档位如下：

| 菜单选项 | 处理方式 |
| --- | --- |
| SMAA 1x（标准） | 当前帧的边缘检测与混合。 |
| SMAA 1x（斜线与拐角增强） | 扩大搜索范围，增加斜线和拐角处理。 |
| SMAA T2x（标准） | 标准边缘处理，结合运动矢量对齐两帧采样。 |
| SMAA T2x（斜线与拐角增强） | 两帧采样，增加斜线和拐角处理。 |

显卡支持顶点纹理读取时，TAA 和 T2x 将当前帧、上一帧的变形后位置留在 GPU，
共享输入相同的材质捕获结果，并沿用原索引缓冲。已确认不透明的原生材质省去额外覆盖绘制，
MME 着色器和透明贴图保留覆盖检查。程序按实际能力检测 NVIDIA、AMD 和 Intel，
不支持顶点纹理读取时保留 CPU 几何捕获。复杂 MME 场景仍有额外开销，SMAA 1x 不需要运动捕获。

只有第 0 帧的工程也能输出时间抗锯齿图片。程序在同一场景时间补足缺少的采样，
不推进动作或物理，不增加输出图片。连续图片序列保留历史：T2x 首张需要两次场景渲染，
后续每张一次。硬件或效果不支持运动矢量时，回退到单帧处理。

抗锯齿开关、当前算法和上次选择的 SMAA 档位随 PMM v2 扩展保存。加载工程不会改写
本机默认设置。临时硬件回退和未知档位不会覆盖工程原记录，用户重新选择后才更新。
具体格式见 [PMM v2 项目扩展](docs/reference/pmm-extensions.md)。

需要此修复时，单独下载 `MMSNvidiaAAFix.dll`，放到完整版的 `MikuMikuDance.exe` 同目录，通过「帮助」菜单管理补丁。放置 DLL 不会自动安装驱动设置。打开菜单时，DLL 查询当前程序的设置，并显示安装或卸载选项。两种操作均请求 Windows 管理员授权，下次启动 MMS 时生效。

此设置只应用于 NVIDIA；没有可用 NVIDIA 驱动时停止安装，AMD、Intel 不应用该设置。卸载前会提示 MSAA 白边或接缝可能再次出现，卸载仅移除当前程序的设置。删除 DLL 会隐藏菜单入口，但不会撤销已安装的驱动设置。

### 曲线编辑器

原生曲线编辑器支持骨骼位移、XYZ 角度和缩放的九通道曲线，也可编辑表情、相机、照明、自阴影、重力和附件轨道。支持保持曲线形状的插帧、控制点数值输入、斜率与权重拖动及关键帧跳转。

源码与 Git 历史核对见[MMS 原生功能审计](docs/records/development/features.md#record-mms-native-features-audit)。

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

MMS 最终发布包固定在 `D:/MMD-Next/MMS/build/Release/`。移动仓库目录后，先重新运行
`conan install`，再使用 CMake 预设，避免沿用旧目录的绝对路径。构建完成后，主要文件如下：

| 路径                              | 用途                                 |
| --------------------------------- | ------------------------------------ |
| `build/Release/MikuMikuDance.exe` | GUI 主程序                           |
| `build/lib/`                      | 静态库和导入库                       |
| `build/symbols/`                  | 用于调试和崩溃定位的 PDB 符号文件    |

运行前，还需将 `toon01.bmp` 至 `toon10.bmp` 等 toon 纹理放在**工作目录**的 `Data/` 下。
例如，以 `build/Release/` 为工作目录启动时，纹理应放在 `build/Release/Data/`。

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
cmake --preset conan-default -DMIKUDANCESTUDIO_BUILD_INPUT_TESTS=ON
cmake --build build --config Release --target keyboard_input_tests
ctest --test-dir build -C Release -R keyboard_input_regressions --output-on-failure
```

### MikuMikuEffect（MME）支持

#### 安装与使用

将 **x64 版 MME** 的以下三个文件放到 `MikuMikuDance.exe` 同一目录：

- `d3d9.dll`
- `MMHack.dll`
- `MMEffect.dll`

启动后，菜单栏会出现 `MMEffect` 菜单，可按 MME 的常规方式为模型和配件分配 `.fx` 特效。
请保留主程序名 `MikuMikuDance.exe`，MME 依赖这个名称识别宿主。
本项目的构建与运行均不要求安装 DirectX SDK。

#### 兼容实现说明

本移植为 MME 提供了以下宿主接口：

- **导出函数**：提供 37 个 `Exp*` 函数，导出序号为 1–37，定义见
  [`exports/MikuMikuDance.def`](exports/MikuMikuDance.def)。
- **D3DX 静态导入**：为 `MMHack.dll` 提供可挂钩的导入地址表（IAT）条目，
  导入库由 [`res/imports/d3dx9_43.def`](res/imports/d3dx9_43.def) 在构建时生成。
- **宿主名称**：使用 `MikuMikuDance.exe`，与 `MMHack.dll` 引用的模块名一致。

对象登记、效果分配及已验证的兼容范围，见 [MME 使用与兼容说明](docs/guides/effects.md)。

### 个人习惯更改

特性添加仅为方便本人使用，非MMD原本使用习惯或适配需求，编译者可编辑代码删去。

- **原生骨骼缩放**：所有骨骼支持整体及 XYZ 缩放，统一使用局部、全局和视图坐标系。PMM 保存完整数据，VMD 在保留标准前缀的同时，将缩放写入同一文件尾部。见[骨骼缩放说明](docs/guides/bone-scaling.md)。
- **扩展自阴影产生范围**：取消原版 MMD 对自阴影产生范围的限制。
- **动态缩放**：按可见几何的投影密度匹配阴影贴图分辨率，受美影设置和显卡上限限制。动态与固定精度共用同一阴影范围滑条，不再保存独立精度值；完整包在高精度下保留覆盖范围，减少裁切。
- **错误提示**：程序默认不写日志文件，可显式开启诊断文件跟踪。遇到致命错误时会弹出对话框，可按 `Ctrl+C` 复制内容。
  开发者可结合 `build/symbols/` 下对应构建的 PDB 定位源码。
- **输入法与快捷键**：开启中文或日文输入法时，字母快捷键仍有效，包括 `P`（播放/停止）
  和 `A/S/D/F/G/H/I/J/K/L/U/V/X/Z/B/C/R`；在编辑框内输入文字时除外。
- **透明排序偏好**：增加关闭、低、标准、高、最高五档，便于按个人场景调整透明排序。

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
- 老面星星（StarNoodle495，[BowlRoll](https://bowlroll.net/user/182602)）：感谢一直以来持续进行测试并提出建议；感谢沙拉酱（[Bilibili](https://space.bilibili.com/3493080791517295)）提供技术指导；感谢微波叮桃子（[AplayBox](https://www.aplaybox.com/u/972840773)）、靡苦居士（[Bilibili](https://space.bilibili.com/8545990)）、幽灵_U0（[Bilibili](https://space.bilibili.com/454701392)）和低智商鼻嘎参与测试并提交 bug。

### 许可

本工程采用与 MikuMikuDance 相同的免费软件条款，详见 [LICENSE](LICENSE)。
第三方组件遵循各自许可：Bullet Physics（zlib）、Alembic 与 Imath（BSD-3-Clause）、MinHook（BSD-2-Clause）、DirectX SDK / Windows SDK（微软 EULA）。新增导出依赖的许可文本见 [licenses](licenses/README.md)。

---

## 日本語

MikuMikuStudio（MMS）は、MMD のダンスアニメーションを制作・編集する Windows デスクトップアプリです。以下は現在の完全版ソースの説明です。完全版・最小版の範囲と形式の互換性は[ダウンロードとパッケージの違い](#ダウンロードとパッケージの違い)を参照してください。

### 機能

- **モデル**：PMD / PMX（BDEF4/SDEF ウェイト対応）読み込み、ボーンツリー、IK、
  モーフ、トゥーンレンダリング
- **アニメーション**：VMD 読み込み／保存、VPD エクスポート、フレームエディタ
  （ボーン／モーフ／カメラ／照明／自作表情／アクセサリトラック）、物理プレビュー付き
  フレーム再生、プロジェクト全体のフレームレート修正
- **物理**：Bullet 2.75 剛体とコンストレイント（6DOF スプリング）、決定論的な求解順序。物理結果のベイクとボーン動作のフレーム単位再サンプリングにも対応します。フレーム範囲、ERP、CFM、サブステップ、計算周波数を設定でき、取り消し・キャンセルと暗号化しない UTF-8 JSON プリセットを利用できます。UI とツールチップは日英中に対応し、初期値は Bullet の既定値です。[物理ベイク](docs/guides/physics-baking.md)
- **メッシュベイクと出力**：Alembic は MME の頂点変形を含むメッシュを出力します。ABC 出力とプロジェクトへのベイクは個別に選択でき、ベイクを選んだ場合だけ影響を警告します。メッシュベイク解除で選択モデルまたは全オブジェクトの変形を元に戻し、無効化済み FX は無効のままです。[メッシュ出力・ベイク](docs/guides/mesh-export.md)
- **レンダリング**：ネイティブ DX12 プレビューと GPU スキニングに対応し、DX12 が使えない場合は DX9/CPU にフォールバックします。MME Effect、トゥーンテクスチャ、地面影、立体視（NVIDIA 3D Vision）をサポートし、メイン描画と頂点計算のデバイスを個別に選べます。
- **マルチメディア**：Windows Media Foundation を使って多くの音声形式と MP4/MOV 動画を読み込みます。利用可能な形式は Windows のメディアコンポーネントに依存します。Wave 再生、インストール済みコーデックを使う DirectShow AVI、Media Foundation による MP4/H.264、透過 MOV の録画にも対応します。出力画面で登録済み H.264 エンコーダーを確認でき、縦横比を保った解像度変更も可能です。VSQ と Kinect 入力も利用できます。
- **UI**：英語・中国語・日本語 UI、タイムライン、
  Unicode ファイルパス、アクセサリ編集、元に戻す／やり直し

#### 編集容量の自動拡張

名前は完全な Unicode 文字列を保持します。ボーン、モーフ、表示、カメラ、照明、セルフシャドウ、重力、アクセサリのキーフレーム容量は必要に応じて拡張します。表示グループとモーフ表示項目も、従来の 1 バイトの件数制限を超えて保持します。容量の手動設定は不要です。

フレーム番号はオーバーフローを検査し、2,147,483,646 まで扱います。大きなフレーム番号でも隣接キーの再生時刻を区別します。自動拡張は利用可能なメモリとファイルの表現範囲に制約されます。保存と旧版互換の規則は [PMM と VMD の違いと互換性](#pmm-と-vmd-の違いと互換性)を参照してください。

#### 連番画像と自動バックアップ

両パッケージで連番画像を出力できます。保存先、ファイル名の接頭辞、画像形式、フレーム範囲を設定でき、形式は単一画像出力と共通です。連番の桁数は出力範囲に合わせて調整します。

自動保存設定では、間隔（分）、保存先、保存数の上限、古いバックアップの上書きを設定できます。既定の保存先は実行ファイルと同じ場所の `backups` で、手動保存したプロジェクトは上書きしません。クラッシュ時にもバックアップを試みますが、完了するかどうかはプログラムの状態や保存先への書き込み可否に依存します。

#### ビューポートと影の設定

完全版はウィンドウサイズへの自動調整、低解像度プレビュー、スーパーサンプリングに対応し、出力サイズは変更しません。低解像度画像は最近傍補間で拡大し、HUD とツールバーは黒帯を含む領域に表示解像度で描画します。数値入力欄の幅を先に詰め、文字の大きさをできるだけ保ちます。ツールバーが一行に収まらず、隣接する黒帯に余裕がある場合は二行に配置します。FPS 表示と HUD は通常の大きさを保ち、プレビュー領域に収まらない場合に限り縮小します。FPS は描画が完了し、正常に送出されたシーンフレームを数えます。

セルフシャドウは MMD 9.32 の投影と深度サンプリングを復元し、動的精度と固定精度で同じ影範囲設定を使います。「フィルター強化」は既定で無効です。有効時は影マップに 2 パスの B スプラインフィルターを適用してから、標準材質と MME に渡します。失敗時はそのフレームの元の影マップを使います。

#### アンチエイリアスとプロジェクト設定

完全版は MMD オリジナル（MSAA）、FXAA、モーションベクトルを使う TAA、SMAA に対応します。
現在のソースでは、「SMAA」はサブメニューを開くだけで、既定の選択はありません。
サブメニューで 1x と T2x、それぞれの標準・斜線とコーナーの強化を選びます。
SMAA を選択している場合だけ、「SMAA」と現在の方式の両方に選択マークを表示します。
T2x はモーションベクトルで位置を合わせた 2 フレームのサンプルを合成します。
頂点テクスチャ参照に対応する GPU では、TAA と T2x の現在・前フレームの変形後座標を GPU 上に保持します。
入力が一致する材質で取得結果を共有し、元のインデックスバッファを再利用します。
不透明と確認できた標準材質では追加のカバレッジ描画を省き、MME とアルファ付きテクスチャでは確認を維持します。
NVIDIA、AMD、Intel を実際の機能で判定し、頂点テクスチャ参照に非対応の場合は CPU での取得を維持します。
複雑な MME には追加の負荷が残ります。SMAA 1x はモーション取得を行わず、モーションに対応しない描画は空間 AA に切り替えます。

フレーム 0 だけの画像出力でも、時刻と姿勢を保ったまま不足するサンプルを描画します。
アニメーションや物理を進めず、画像数も増やしません。連続画像では履歴を保つため、
T2x の最初の画像は 2 回、以後は画像ごとに 1 回だけシーンを描画します。
AA の有効状態、方式、前回の SMAA 選択は PMM v2 拡張に保存されます。
読み込み時にローカルの既定設定を書き換えません。

必要な場合のみ `MMSNvidiaAAFix.dll` を別途ダウンロードし、完全版の `MikuMikuDance.exe` と同じフォルダーに置いてください。配置しただけではドライバー設定は変更されません。ヘルプメニューを開くと DLL が現在の設定を確認し、インストールまたはアンインストールを表示します。どちらも Windows の管理者権限を要求し、次回の MMS 起動時に反映されます。

この設定は NVIDIA 専用です。利用可能な NVIDIA ドライバーがない場合はインストールを中止し、AMD・Intel には適用しません。アンインストール前に MSAA の白い縁や継ぎ目が再発する可能性を表示し、現在のプログラムの設定だけを解除します。DLL の削除はメニュー項目を非表示にするだけで、適用済みの設定は解除しません。

### 曲線エディタ

ネイティブの曲線エディタで、ボーンの移動、XYZ 回転、拡大縮小の 9 チャンネルに加え、モーフ、カメラ、照明、セルフシャドウ、重力、アクセサリのトラックを編集できます。形状を維持するキー挿入、制御点の数値入力、傾きとウェイトのドラッグ、キーフレームへの移動に対応します。

ソースと Git 履歴の根拠は[MMS ネイティブ機能監査](docs/records/development/features.md#record-mms-native-features-audit)を参照してください。

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

実行ファイルはオリジナルと同じ `MikuMikuDance.exe` 名で、MME がホストに要求する 3 点を
満たします：37 個の `Exp*` エクスポート（序数 1..37）、MMHack.dll が書き換える
`d3dx9_43.dll` の IAT スロットを**静的インポート**で用意（`res/imports/d3dx9_43.def` から
ビルド時にインポートライブラリを生成。以前は D3DX を動的解決していたため書き換え先がなく
`Initialize error` になっていました）、そしてモジュール名そのもの（MMHack.dll のインポート表が
`MikuMikuDance.exe` から `Exp*` を束縛します）。x64 版の `d3d9.dll`、`MMHack.dll`、
`MMEffect.dll` を同じフォルダに置くとメニューバーに `MMEffect` 欄が出て、あとは通常どおり
`.fx` を割り当てるだけです。実行時もビルド時も DirectX SDK は不要です。詳細は
[MME の使い方と互換性](docs/guides/effects.md)。

### 個人的な使用習慣による変更

追加機能は個人的な利便性のためのもので、MMD 本来の使用習慣でも互換要件でもありません。
ビルドする方はコードを編集して削除できます。

- **ボーンスケール**：すべてのボーンを一括または XYZ 軸ごとに拡大縮小できます。ローカル・グローバル・ビュー座標に対応し、PMM に保存します。VMD は標準部分を維持し、同じファイルの末尾にスケール拡張を保存します。[詳細](docs/guides/bone-scaling.md)。
- **セルフシャドウ範囲の拡張**：オリジナル MMD にあるセルフシャドウ生成範囲の制限をなくしました。
- **動的スケーリング**：表示中のジオメトリの投影密度に応じて、シャドウ品質と GPU の上限内で影マップの解像度を調整します。動的精度と固定精度は同じ影範囲スライダーを使い、独立した精度値は保存しません。完全版は高精度時も影の範囲を保持して欠けを抑えます。
- **エラー表示**：既定ではログファイルを出力しません。診断用のファイル記録は明示的に有効にできます。致命的エラー時はモーダルなエラーウィンドウを
  表示します（Ctrl+C で内容をコピーでき、`build/symbols/` の PDB でソース行を特定できます）。
- **入力メソッドとショートカット**：中国語／日本語の入力メソッドを ON にしたままでも
  英字ショートカット（P 再生／停止、ほか A/S/D/F/G/H/I/J/K/L/U/V/X/Z/B/C/R）が有効です
  （テキスト入力欄を除く）。
- **透明ソート設定**：個人のシーンに合わせて、無効・低・標準・高・最高の五段階を選べます。

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
- 老面星星氏（StarNoodle495、[BowlRoll](https://bowlroll.net/user/182602)）：継続的なテストと提案に感謝します。沙拉酱氏（[Bilibili](https://space.bilibili.com/3493080791517295)）には技術指導を、微波叮桃子氏（[AplayBox](https://www.aplaybox.com/u/972840773)）、靡苦居士氏（[Bilibili](https://space.bilibili.com/8545990)）、幽灵_U0 氏（[Bilibili](https://space.bilibili.com/454701392)）、低智商鼻嘎氏にはテストとバグ報告に感謝します。

### ライセンス

本プロジェクトは MikuMikuDance と同じフリーウェア条項の下で配布されます。
詳細は [LICENSE](LICENSE) を参照してください。サードパーティコンポーネントは
それぞれのライセンスに従います：Bullet Physics（zlib）、
DirectX SDK / Windows SDK（マイクロソフト EULA）。
