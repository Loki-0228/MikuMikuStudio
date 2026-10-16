# 发布说明与构建记录

本页按功能归并实现、诊断与验收资料。各记录中的「当前」「已完成」只适用于其注明的源码、构建和测试条件；当前使用入口见[文档导航](../README.md)。

## 记录索引
- [v2026.10.17.1 发布说明](#release-v2026-10-17-1)
- [v2026.10.15.2 发布说明](#release-v2026-10-15-2)
- [v2026.10.15.1 发布说明](#release-v2026-10-15-1)
- [MikuDanceStudio v2026.10.14.6 release notes / 发布说明 / リリースノート](#release-v2026-10-14-6)
- [共享阴影滤波修复](#record-release-notes-shared)
- [v2026.10.13.1 发布说明](#release-v2026-10-13-1)
- [v2026.10.12.1 release notes / 发布日志 / リリースノート](#release-v2026-10-12-1)
- [v2026.10.11.3 构建与发布记录](#record-release-3)
- [v2026.10.11.2 构建与发布记录](#record-release-2)
- [MMS 原生媒体导入与导出](#record-native-media)

<a id="release-v2026-10-17-1"></a>

本节为待发布说明；本次仅审查源码和隔离验证，不表示已打包或替换正式程序。

<!-- release-notes:v2026.10.17.1:begin -->
## MikuMikuStudio v2026.10.17.1

Windows x64

[English](#release-v2026-10-17-1-en) | [中文](#release-v2026-10-17-1-zh) | [日本語](#release-v2026-10-17-1-ja)

<a id="release-v2026-10-17-1-en"></a>

### English

This release includes the changes since `v2026.10.15.2`.

#### What's new

- Fixed interpolation and tangent buttons failing to apply changes immediately in the curve editor. Choosing Step, Linear, Bezier, or a tangent setting applies it directly. Changes to multiple selected keys can be undone together.
- Clicking a channel name now keeps subsequent Space and Enter actions on that channel instead of the previously focused row.
- Separated persistent handle modes from one-time curve presets. Free, Aligned, and Automatic control handle editing. Flat, Straighten, Ease in, and Ease out apply once when clicked. Intervals that need handles switch to Bezier. Later numeric edits no longer reapply the preset.
- Fixed small sampling errors at native camera Bezier keyframes. Writing unchanged camera curves back through the editor and saving a PMM no longer introduces these keyframe value offsets.
- Fatal errors now produce timestamped `error-*.log` files in `backups` beside the EXE. Logs include the exception, call stack, recent runtime messages, and backup result. When the scene can be saved safely, MMS also attempts a crash backup there. Disabling periodic autosave leaves crash recovery enabled.
- Added a popup when a backup folder reaches its file limit. It lists full folders and the current overwrite policy. The popup appears once per launch and returns after a restart if a folder is still full. Existing settings still apply: overwrite mode replaces old backups; otherwise, new backups stop in full folders. Playback, export, and other dialogs defer the popup.
- With MME installed, Auto Save Mapping File is checked by default when no preference has been saved. Saving a PMM also saves its effect assignments to the matching `.emm` file. An existing choice to disable this option is preserved.

#### Download and use

Choose the full package for the features above. The minimal package retains the original MMD feature set and encoding limits, without the full build's extended editing and export features. Extract each package into a separate folder and run `MikuMikuDance.exe`. Models, personal settings, MME, MMV, and FFmpeg are not included.

Keep the matching `.emm` file with the PMM when moving or sharing a project that uses MME effects. Crash backups and error logs are stored in `backups` beside the EXE. Periodic autosave continues to use the folder selected in its settings.

#### Optional NVIDIA fix

Neither application package includes the NVIDIA fix. If needed, download `MMSNvidiaAAFix.dll` separately and place it beside the full build's EXE. Install it from Help, then restart MMS. Use Help to uninstall. Deleting the DLL does not remove an installed setting.

<a id="release-v2026-10-17-1-zh"></a>

### 中文

本版本包含自 `v2026.10.15.2` 以来的更新。

#### 更新内容

- 修复曲线编辑器点击插值类型或切线按钮后未立即生效的问题。选择「阶跃」「线性」「贝塞尔」或切线设置后直接应用，多选关键帧可批量修改并整体撤销。
- 修复点击通道名称后，空格和回车仍可能作用于旧通道的问题。
- 区分持续生效的手柄模式与一次性曲线预设。「自由」「对齐」「自动」控制手柄编辑方式；「水平」「拉直」「缓入」「缓出」仅在点击时应用。需要手柄的区间自动切换为贝塞尔，后续数值修改不再重复套用预设。
- 修复原生镜头贝塞尔曲线在关键帧处采样产生微小偏差的问题，避免未修改镜头曲线时，经编辑器写回和 PMM 保存后关键帧数值发生偏移。
- 致命错误日志保存到 EXE 所在目录的 `backups` 文件夹，文件名为带时间戳的 `error-*.log`，记录异常、调用栈、最近运行信息及备份结果。场景状态允许安全保存时，程序还会尝试在该目录生成崩溃备份；关闭定时自动保存后仍保留此功能。
- 备份文件夹达到数量上限时弹窗提醒，并显示已满目录和当前覆盖策略。每次启动仅提示一次，重启后仍满会再次提示。弹窗沿用现有设置：允许覆盖时继续替换旧备份，禁止覆盖时停止向已满目录新增备份。播放、导出或其他对话框占用期间延后提示。
- 安装 MME 后，未配置过的「自动保存效果分配文件」默认勾选。保存 PMM 时同步保存同名 `.emm` 特效分配文件；已有的手动关闭设置继续保留。

#### 下载与使用

通常选择完整包。最小包保留原版 MMD 的功能范围及编码限制，不包含完整包的扩展编辑与导出功能。分别解压到独立目录后运行 `MikuMikuDance.exe`。模型、个人设置、MME、MMV 和 FFmpeg 需自行准备。

移动或分享使用 MME 特效的工程时，需将同名 `.emm` 与 PMM 一并保留。崩溃后可在程序目录的 `backups` 文件夹查找备份和错误日志；定时自动保存仍使用设置中指定的目录。

#### NVIDIA 修复安装（可选）

两个主程序包均不附带 NVIDIA 修复插件。需要时单独下载 `MMSNvidiaAAFix.dll`，放到完整版 EXE 同目录，在「帮助」菜单选择安装并重启 MMS。卸载也通过该菜单操作，删除 DLL 不会撤销已安装的设置。

<a id="release-v2026-10-17-1-ja"></a>

### 日本語

`v2026.10.15.2` 以降の変更を含みます。

#### 更新内容

- カーブエディターで補間方式や接線のボタンをクリックしても、変更がすぐに反映されない問題を修正しました。ステップ・線形・ベジェや接線設定を選ぶと直接適用されます。複数の選択キーへの変更はまとめて元に戻せます。
- チャンネル名をクリックした後、スペースキーと Enter キーが以前の行に作用する問題を修正しました。
- 継続して使うハンドルモードと、一度だけ適用するプリセットを分けました。「フリー」「整列」「自動」はハンドルの編集方法を指定します。「水平」「直線化」「イーズイン」「イーズアウト」はクリック時だけ適用されます。ハンドルが必要な区間はベジェに切り替わり、その後の数値変更でプリセットを再適用しません。
- 原版形式のカメラのベジェ曲線で、キーフレームの値に微小なサンプリング誤差が生じる問題を修正しました。変更していないカメラ曲線をエディターから書き戻して PMM に保存する際、この誤差によってキーの値がずれることを防ぎます。
- 致命的なエラーのログを EXE と同じ場所の `backups` フォルダーに保存します。日時を含む `error-*.log` に例外、呼び出し履歴、直近の動作記録、バックアップ結果を記録します。シーンを安全に保存できる場合は、同じフォルダーへのクラッシュ時バックアップも試みます。定期的な自動保存を無効にしても、この機能は有効です。
- バックアップフォルダーが保存数の上限に達すると、対象フォルダーと現在の上書き設定をポップアップで知らせます。起動ごとに一度だけ表示し、再起動後も上限に達している場合は再表示します。上書きが有効なら古いバックアップを置き換え、無効なら満杯のフォルダーへの新規保存を停止します。再生、出力、ほかのダイアログの操作中は表示を延期します。
- MME を導入した環境では、設定が保存されていない場合に「割当ファイル自動保存」を初期状態で有効にします。PMM の保存時に同名の `.emm` にエフェクトの割り当ても保存します。すでに無効にした設定は維持します。

#### ダウンロードと使い方

通常は完全版を選んでください。最小版は原版 MMD の機能範囲と文字コード制限を維持し、完全版の拡張編集・出力機能を含みません。各パッケージを別のフォルダーに展開し、`MikuMikuDance.exe` を実行します。モデル、個人設定、MME、MMV、FFmpeg は同梱しません。

MME エフェクトを使うプロジェクトを移動・共有する際は、同名の `.emm` を PMM と一緒に保管してください。クラッシュ時のバックアップとエラーログは EXE と同じ場所の `backups` に保存します。定期的な自動保存は設定で指定したフォルダーを引き続き使います。

#### NVIDIA 修正の導入（任意）

NVIDIA 修正プラグインは両方のパッケージに含まれません。必要な場合のみ `MMSNvidiaAAFix.dll` を別途ダウンロードし、完全版の EXE と同じフォルダーに置きます。ヘルプからインストールして MMS を再起動してください。解除も同じメニューから行います。DLL の削除だけでは適用済みの設定は解除されません。
<!-- release-notes:v2026.10.17.1:end -->

<a id="release-v2026-10-15-2-pre-1"></a>
<a id="release-v2026-10-15-2"></a>

<!-- release-notes:v2026.10.15.2:begin -->
## MikuMikuStudio v2026.10.15.2

Windows x64

[中文](#release-v2026-10-15-2-zh) | [English](#release-v2026-10-15-2-en) | [日本語](#release-v2026-10-15-2-ja)

<a id="release-v2026-10-15-2-zh"></a>

### 中文

本版本包含自 `v2026.10.15.1` 以来的更新。

#### 更新内容

- 修复完整包中点击「打开」无法直接弹出文件选择窗口，以及点击「导出图片序列」没有反应的问题。打开菜单右侧箭头仍可展开最近工程。
- 图片序列按视频录制流程连续导出，第 0 帧使用动画摄像机位置。特效和物理按输出帧率推进，同一帧的抗锯齿预热不重复推进特效时间，避免磁盘写入耗时导致画面不连贯。
- 「导出图片序列」窗口布局更紧凑，默认帧范围自动取自场景中的有效关键帧和音频时长，无需额外检测按钮。统一中文菜单与窗口用语，并修复单张图片导出窗口在中文、英文和日文模式下的标题乱码。
- 修复完整包中动作和镜头文件位于含中文等 Unicode 字符的文件夹时无法导入的问题。最小包继续遵守原版 CP932 名称和路径限制，并在导入时检查名称、引用和字段长度。
- 抗锯齿子菜单新增「关闭」，与一级开关同步。关闭后记住上次算法，再次开启时恢复；直接选择算法也会开启抗锯齿。
- 调整原生细描边的覆盖和透明度，减少粗细偏差，并改进 TAA、SMAA T2x 的静止边缘稳定性。已回退导致画面明显变模糊的 FXAA 实验。
- 修复停止音频时的线程状态竞争，以及音频线程启动失败后可能无法结束的问题。
- 保留 MME 特效指定的固定阴影贴图尺寸，修复附件绘制顺序和编辑选中项的保持问题。

#### 下载与使用

通常选择完整包。最小包保留原版 MMD 的功能范围及编码限制，不包含完整包的扩展编辑与导出功能。分别解压到独立目录后运行 `MikuMikuDance.exe`。模型、个人设置、MME、MMV 和 FFmpeg 需自行准备。

#### NVIDIA 修复安装（可选）

两个主程序包均不附带 NVIDIA 修复插件。需要时单独下载 `MMSNvidiaAAFix.dll`，放到完整版 EXE 同目录，在「帮助」菜单选择安装并重启 MMS。卸载也通过该菜单操作，删除 DLL 不会撤销已安装的设置。

<a id="release-v2026-10-15-2-en"></a>

### English

This release includes the changes since `v2026.10.15.1`.

#### What's new

- Fixed clicks on **Open** failing to show the file picker and **Export image sequence** doing nothing in the full build. The arrow beside **Open** still opens recent projects.
- Image sequences now follow continuous video recording, including the animated camera at frame 0. Effects and physics advance at the output frame rate; AA warm-up samples do not advance effect time again within a frame. Disk write delays no longer change the export clock.
- Made the image-sequence dialog more compact. Its default range comes from active scene keyframes and audio duration, with no separate detection button. Unified the Chinese export labels and fixed the single-image export window title in Chinese, English, and Japanese.
- Fixed motion and camera imports from folders containing Chinese or other Unicode characters in the full build. The minimal build retains the original CP932 limits and now validates names, references, and field lengths during import.
- Added **Off** to the AA submenu and synchronized it with the main toggle. Turning AA off remembers the selected algorithm; turning it back on restores that selection. Choosing an algorithm also enables AA.
- Adjusted native thin-outline coverage and opacity to reduce thickness differences, and improved stationary-edge stability in TAA and SMAA T2x. Reverted the FXAA experiments that made the image noticeably blurrier.
- Fixed a thread-state race when stopping audio and a shutdown stall when the audio thread could not start.
- Preserved fixed shadow-map sizes specified by MME effects and fixed accessory draw-order and editing-selection retention.

#### Download and use

Choose the full package for the features above. The minimal package retains the original MMD feature set and encoding limits, without the full build's extended editing and export features. Extract each package into a separate folder and run `MikuMikuDance.exe`. Models, personal settings, MME, MMV, and FFmpeg are not included.

#### Optional NVIDIA fix

Neither application package includes the NVIDIA fix. If needed, download `MMSNvidiaAAFix.dll` separately and place it beside the full build's EXE. Install it from **Help**, then restart MMS. Use **Help** to uninstall. Deleting the DLL does not remove an installed setting.

<a id="release-v2026-10-15-2-ja"></a>

### 日本語

`v2026.10.15.1` 以降の変更を含む正式版です。

#### 更新内容

- 完全版で「開く」をクリックしてもファイル選択画面が表示されない問題と、連番画像出力が反応しない問題を修正しました。「開く」の右側の矢印からは引き続き最近使ったプロジェクトを選べます。
- 連番画像を動画録画と同じ連続処理で出力し、0 フレーム目にもアニメーションのカメラを適用します。エフェクトと物理演算は出力フレームレートに従って進み、同一フレームの AA 初期化でエフェクト時間が重複して進むことを防ぎます。ディスクへの書き込み時間は出力時刻に影響しません。
- 連番画像出力画面をコンパクトにしました。出力範囲の初期値は有効なシーンのキーフレームと音声の長さから求め、検出ボタンは不要です。中国語の表記を統一し、単一画像出力ウィンドウの中国語・英語・日本語タイトルの文字化けを修正しました。
- 完全版で中国語などの Unicode 文字を含むフォルダーからモーションやカメラを読み込めない問題を修正しました。最小版は原版の CP932 制限を維持し、読込時に名前・参照先・フィールド長を検証します。
- AA サブメニューに「オフ」を追加し、親メニューの切り替えと連動させました。オフにしても選択した方式を記憶し、再度オンにすると復元します。方式を選ぶ操作でも AA が有効になります。
- 原生の細い輪郭線のカバレッジと不透明度を調整して太さの差を抑え、TAA と SMAA T2x の静止したエッジの安定性を改善しました。画像が大きくぼける FXAA の実験的変更は取り消しました。
- 音声停止時のスレッド状態の競合と、音声スレッドの起動失敗時に終了できなくなる問題を修正しました。
- MME エフェクトが指定した固定シャドウマップサイズを維持し、アクセサリの描画順と編集中の選択の保持を修正しました。

#### ダウンロードと使い方

通常は完全版を選んでください。最小版は原版 MMD の機能範囲と文字コード制限を維持し、完全版の拡張編集・出力機能を含みません。各パッケージを別のフォルダーに展開し、`MikuMikuDance.exe` を実行します。モデル、個人設定、MME、MMV、FFmpeg は同梱しません。

#### NVIDIA 修正の導入（任意）

NVIDIA 修正プラグインは両方のパッケージに含まれません。必要な場合のみ `MMSNvidiaAAFix.dll` を別途ダウンロードし、完全版の EXE と同じフォルダーに置きます。ヘルプからインストールして MMS を再起動してください。解除も同じメニューから行います。DLL の削除だけでは適用済みの設定は解除されません。
<!-- release-notes:v2026.10.15.2:end -->

<a id="release-v2026-10-15-1-pre-1"></a>
<a id="release-v2026-10-15-1"></a>

<!-- release-notes:v2026.10.15.1:begin -->
## MikuMikuStudio v2026.10.15.1

Windows x64

[中文](#release-v2026-10-15-1-zh) | [English](#release-v2026-10-15-1-en) | [日本語](#release-v2026-10-15-1-ja)

<a id="release-v2026-10-15-1-zh"></a>

### 中文

本版本包含自 `v2026.10.14.6` 以来的更新。

#### 更新内容

- 完整包支持导入常见的 8-bit H.264 MP4。视频在后台解码，拖动时间轴时可以继续操作，背景画面可能稍后跟上。
- 按住 Ctrl 拖动视口右上角的十字箭头或放大镜，可以移动或缩放背景。图片和视频共用此操作，位置与缩放比例随工程保存。
- 完整包的「编辑」菜单新增「选中帧序列散布」。选定一段表情关键帧后，可设置插入范围，以及间隔和播放速度的均值、方差。散布保留原片段、跳过已有关键帧冲突，并支持一次撤销。
- 完整包在原有骨骼位置与角度调整窗口下方增加 XYZ 缩放乘数和偏移量，支持与位置、角度一起撤销。
- 修复播放范围为空时「播放」按钮和 P 键无效、新建工程时坐标显示不一致、Track 镜头偏移、切换模型后方向键误切模型，以及重开工程后显示分组意外展开。
- 修复框选残留旧选择、删除时连带删除此前选中的骨骼关键帧，以及表情、显示、IK 和外部亲关键帧的撤销问题。撤销最近一次删除时，不再恢复更早删除的关键帧。
- 优化大量缩放关键帧的播放、曲线选区同步、模型缓存和 MME 重载。完整版的部分特效阴影随预览分辨率调整，以减少预览开销。
- 完整包按需扩展编辑容量，保存 PMM 时优先使用原版格式。超出原版容量或路径编码限制时使用 MMS 扩展格式。

#### 下载与使用

通常选择完整包。最小包保留原版 MMD 的功能范围，不含散布、骨骼缩放、自由曲线或现代视频解码，背景视频仅支持 AVI。分别解压到独立目录后运行 `MikuMikuDance.exe`。模型、个人设置、MME、MMV 和 FFmpeg 需自行准备。

完整版视频预览的长边最多为 1024 像素，高位深视频转换为 8-bit RGB。录制和图片序列输出会等待准确帧。Windows 无法解码的格式（如部分 10-bit H.264）需要另行提供 FFmpeg，可将 `ffmpeg.exe` 放在程序旁的 `tools` 文件夹、程序同目录，或加入 `PATH`。

#### NVIDIA 修复安装（可选）

两个主程序包均不附带 NVIDIA 修复插件。需要时单独下载 `MMSNvidiaAAFix.dll`，放到完整版 EXE 同目录，在「帮助」菜单选择安装并重启 MMS。卸载也通过该菜单操作，删除 DLL 不会撤销已安装的设置。

<a id="release-v2026-10-15-1-en"></a>

### English

This release includes the changes since `v2026.10.14.6`.

#### What's new

- The full build supports common 8-bit H.264 MP4 imports. Videos decode in the background, so you can continue editing while dragging the timeline. The background image may catch up shortly afterward.
- Ctrl-drag the viewport's pan or zoom button to move or resize background images and videos. PMM saves their placement.
- The full build adds **Scatter selected frame sequence** to **Edit**. Copy selected morph keyframes into a target range with configurable means and variances for insertion intervals and playback speeds. The operation preserves the source, skips conflicting placements, and supports one-step undo.
- XYZ scale multipliers and offsets appear below the existing position and angle controls in the full build. Undo restores all three together.
- Fixed empty playback bounds disabling **Play** and **P**, coordinate visibility in new projects, **Track** camera drift, arrow keys switching models after model selection, and display groups expanding after reopening a project.
- Fixed stale box selections, deletion affecting previously selected bones, and undo for morph, display, IK, and external-parent keys. Undoing the latest deletion no longer restores keys from an earlier deletion.
- Optimized playback with many scale keys, curve selection synchronization, model caching, and MME reloads. Some effect shadows in the full build follow the preview resolution to reduce preview overhead.
- The full build expands editing capacity as needed and prefers the original PMM format when saving. Projects beyond the original capacity or path-encoding limits use the MMS extended format.

#### Download and use

Choose the full package for the features above. The minimal package retains the original MMD feature set, without scatter, bone scaling, free curves, or modern video decoding. Its background videos are limited to AVI. Extract each package into a separate folder and run `MikuMikuDance.exe`. Models, personal settings, MME, MMV, and FFmpeg are not included.

Full-build video previews are limited to 1024 pixels on the longer edge and converted to 8-bit RGB. Recording and image-sequence export wait for the exact frame. Formats unsupported by Windows, including some 10-bit H.264 files, require FFmpeg. Place `ffmpeg.exe` in the application's `tools` folder, beside the EXE, or on `PATH`.

#### Optional NVIDIA fix

Neither application package includes the NVIDIA fix. If needed, download `MMSNvidiaAAFix.dll` separately and place it beside the full build's EXE. Install it from **Help**, then restart MMS. Use **Help** to uninstall. Deleting the DLL does not remove an installed setting.

<a id="release-v2026-10-15-1-ja"></a>

### 日本語

`v2026.10.14.6` 以降の変更を含む正式版です。

#### 更新内容

- 完全版で一般的な 8-bit H.264 MP4 を読み込めるようになりました。動画をバックグラウンドで処理するため、タイムラインをドラッグしながら編集を続けられます。背景の表示が一時的に遅れる場合があります。
- Ctrl を押しながらビューポートの移動・拡大縮小ボタンをドラッグすると、背景画像や動画の位置と大きさを変更できます。設定は PMM に保存されます。
- 完全版の編集メニューに選択フレーム列の散布を追加しました。選択した表情キーフレームを、間隔と速度の平均・分散に従って指定範囲に挿入します。元の列を残し、既存キーとの衝突を避け、操作全体を一度で元に戻せます。
- 完全版の既存の位置・角度調整ダイアログ下部に XYZ スケールの乗数と加算値を追加しました。位置・角度とまとめて元に戻せます。
- 再生範囲が空の場合に再生ボタンと P キーが反応しない問題、新規プロジェクトの座標表示、Track のカメラずれ、モデル選択後に矢印キーで別のモデルに切り替わる問題、再読込時に表示グループが展開される問題を修正しました。
- 範囲選択に以前の選択が残る問題や、以前選択したボーンのキーまで削除される問題を修正しました。表情・表示・IK・外部親の削除の取り消しも修正し、直前の削除を取り消したときに、それより前に削除したキーまで復元されないようにしました。
- 多数のスケールキーの再生、曲線の選択同期、モデルキャッシュ、MME 再読込を最適化しました。完全版では一部のエフェクトの影をプレビュー解像度に合わせ、プレビューの負荷を抑えます。
- 完全版では必要に応じて編集容量を拡張し、PMM 保存時は原版の形式を優先します。原版の容量やパスの文字コード制限を超える場合は MMS 拡張形式を使います。

#### ダウンロードと使い方

通常は完全版を選んでください。最小版は原版 MMD の機能範囲を維持し、散布、ボーンスケール、自由曲線、新しい動画形式の読み込みを含みません。背景動画は AVI のみです。各パッケージを別のフォルダーに展開し、`MikuMikuDance.exe` を実行します。モデル、個人設定、MME、MMV、FFmpeg は同梱しません。

完全版の動画プレビューは長辺 1024 ピクセルまでで、8-bit RGB に変換します。録画と連番画像出力では正しいフレームを待ちます。Windows が対応しない一部の 10-bit H.264 などには FFmpeg が必要です。`ffmpeg.exe` をアプリの `tools` フォルダーか EXE と同じフォルダーに置くか、`PATH` に追加してください。

#### NVIDIA 修正の導入（任意）

NVIDIA 修正プラグインは両方のパッケージに含まれません。必要な場合のみ `MMSNvidiaAAFix.dll` を別途ダウンロードし、完全版の EXE と同じフォルダーに置きます。ヘルプからインストールして MMS を再起動してください。解除も同じメニューから行います。DLL の削除だけでは適用済みの設定は解除されません。
<!-- release-notes:v2026.10.15.1:end -->

<a id="release-v2026-10-14-6"></a>
## MikuDanceStudio v2026.10.14.6 release notes / 发布说明 / リリースノート

原始记录：`RELEASE_NOTES_20261014.md`。其中的版本、日期和验证范围沿用原记录。

[English](#release-v2026-10-14-6-english) | [中文](#release-v2026-10-14-6-中文) | [日本語](#release-v2026-10-14-6-日本語)

<a id="release-v2026-10-14-6-english"></a>
### English

Windows x64. These notes cover the cumulative changes since the previous official release, [v2026.10.12.1](https://github.com/Loki-0228/MikuDanceStudio/releases/tag/v2026.10.12.1).

| Item | Previous official release | This release |
| --- | --- | --- |
| Version | `v2026.10.12.1` | `v2026.10.14.6` |
| Commit | `1b80cbf02b884890abb8939715fbbb258d7428e9` | Pinned by the `v2026.10.14.6` tag |
| Status | Published on 2026-10-12 | Published on 2026-10-14 |

The comparison covers all changes since the previous official release and describes the final behavior in this version. The full and minimal packages use the same fixed source revision and exclude uncommitted workspace changes.

<a id="release-v2026-10-14-6-export-backups-and-editing"></a>
#### Export, backups, and editing

- Added image sequence export with output directory, filename prefix, image format, and frame range settings. It supports the formats already available for single-image export. The number of digits in filenames adjusts to the output range.
- Added auto-save settings for the interval in minutes, backup directory, maximum number of backups, and replacement of older backups. The default directory is `backups` beside the application. Manually saved projects are not overwritten. The application always attempts a backup after a crash; success depends on the crash state and whether the destination is writable.
- `Ctrl+F` force paste now supports pasting values between different morphs. The full package adds a force-paste button on the timeline and a recent-projects menu.
- The full package adds independent curves for nine bone channels: position, XYZ rotation, and scale, with playback, seeking, and PMM and VMD extension storage. The curve editor adds double-click key insertion that preserves the curve shape, numeric control-point input, slope and weight dragging, keyframe navigation, and sidebar adjustment.
- Fixed incorrect rotation angles and directions when loading VMD motion for fixed-axis bones. The full package also fixes extra deformation when a child bone rotates under a parent with nonuniform scale.
- Fixed menu state and drawing boundaries after deleting, switching, or reloading accessories. The bottom panel supports width adjustment by dragging, with the layout applied on mouse release. This also fixes some controls extending outside their bounds, flickering, or leaving stale images.

<a id="release-v2026-10-14-6-preview-and-anti-aliasing"></a>
#### Preview and anti-aliasing

- The full package adds viewport render scaling, with automatic window matching, reduced-resolution preview, and supersampling. Export dimensions remain unchanged. Low-resolution previews use nearest-neighbor upscaling to display the pixels directly.
- The HUD, frame rate, and upper and lower toolbars use the entire preview area, including letterbox bars. They render at display resolution after the scene is upscaled. Fixed repainting in the main window, floating viewport, and settings windows obscuring the HUD.
- FPS now counts scene frames that actually finish rendering and are successfully submitted. It excludes idle iterations used to save processing time, repeated submissions, and offline recording samples.
- The anti-aliasing menu offers MMD original (MSAA), FXAA, TAA, and SMAA. SMAA has four options: 1x and T2x, each with standard edges or diagonal and corner enhancement. Clicking SMAA applies the last choice; the initial choice is SMAA 1x with diagonal and corner enhancement. Hover over SMAA and move right to open the options. Separate MSAA levels in older projects remain readable.
- TAA and SMAA T2x add motion-vector reprojection for the camera, bones, and deformed geometry, with depth and history validation. Current color, historical color, and depth use their corresponding sample offsets. When history is rejected, the current offset is removed. Motion capture also includes the ground grid.
- When the GPU supports vertex texture sampling, TAA and SMAA T2x retain both frames of deformed positions on the GPU, avoiding a CPU readback and upload of the captured results each frame. Materials with identical inputs share captures, and verified opaque native materials skip the extra coverage draw. MME and alpha textures retain coverage checks. Devices without this capability use the compatibility path. SMAA 1x behavior is unchanged.
- Static images from projects containing only frame 0 receive the missing temporal samples at the same time and pose, without advancing physics or producing extra images. Continuous image sequences retain history: SMAA T2x renders the first image twice and each subsequent image once. If motion data is unavailable, it falls back to single-frame anti-aliasing.
- The anti-aliasing switch, algorithm, and last SMAA option are saved in the PMM v2 extension. Loading a project does not overwrite local defaults. Temporary hardware fallback does not replace the project's original choice.
- Fixed the viewport-scale menu accidentally triggering Open scene, flicker when opening recent projects, and incorrect restoration of viewport dimensions and DPI state after export. Viewport tools support continuous relative dragging and no longer pull the cursor back after losing capture.

<a id="release-v2026-10-14-6-shadows-and-compatibility"></a>
#### Shadows and compatibility

- Full-package self-shadowing restores the light-grid orientation, projection transform, continuous depth gradients in modes 1 and 2, and linear depth sampling from MMD 9.32. Dynamic shadow resolution shares one range setting, allocates textures according to the actual projection density, and preserves the high-resolution range to avoid clipping.
- The full package adds stronger shadow filtering, disabled by default. When enabled, two passes of B-spline filtering process the current frame's shadow depth map before native materials, MME, and MMV versions supporting the new interface receive it. This prevents external FX from missing the filtered result and avoids filtering it twice in the host. Failure falls back to the current frame's original depth map.
- Fixed the full package still separating opaque and transparent materials under MME when transparency sorting was disabled. With sorting off, materials now draw in the model's original order.
- Fixed ground shadows becoming too dark on some materials after enabling MME. Native rendering and MME now read the same shadow color. Existing shadow brightness, alpha blending switches, and alpha values remain unchanged.
- Fixed the full package's highest transparency-sorting level treating precompiled effects as source text to append to. Transparency depth peeling now uses a separate, complete effect resource, avoiding loading failures, missing transparent layers, or crashes caused by corrupted bytecode.
- Improved original MME compatibility for read-only access to Chinese paths and existing effect references. When the full package sends model paths to MME, it prefers paths that the system code page can represent accurately, improving lookup of EMD files with the same base name.
- The full package includes the optional `MMSNvidiaAAFix.dll` for installing an NVIDIA compatibility setting for double-sided rendering in the current application. When the DLL is present, the Help menu offers installation or removal according to the detected state and requests Windows administrator permission when run. Installation or removal takes effect the next time MMS starts. Deleting the DLL removes the menu entry but does not undo an installed driver setting. Installation stops if no NVIDIA driver is available; AMD and Intel do not receive this setting.
- New settings and prompts support Chinese, Japanese, and English, with revised Japanese and English wording, button labels, and window layouts.

<a id="release-v2026-10-14-6-full-and-minimal-packages"></a>
#### Full and minimal packages

| Added feature or change | Full | Minimal |
| --- | --- | --- |
| Image sequences, periodic backups, and crash backup attempts | Included | Included |
| Morph force paste with `Ctrl+F` and bottom-panel width adjustment | Included | Included |
| Shared fixes for fixed-axis VMD motion, ground-shadow color, accessory state, and export viewport restoration | Included | Included |
| Recent projects, force-paste button, and viewport render scaling | Included | Not included |
| Nine-channel bone curves and nonuniform-scale fix | Included | Bone scaling and the new curve module are not included |
| Dynamic shadows, clipping prevention, and stronger filtering | Included | Retains original shadow rendering and range clipping |
| AA algorithm selection, TAA, SMAA, and NVIDIA patch DLL | Included | Retains the original AA switch; new modules are not included |
| Effect-loading fix for the highest transparency-sorting level | Included | Transparency sorting is not included |

The full ZIP contains `MikuMikuDance.exe`, `MMSNvidiaAAFix.dll`, and `licenses/`. The minimal ZIP contains the application and licenses. Neither package includes models, user settings, or MME. The main ZIPs do not include `MMV4MMS.dll`.

<a id="release-v2026-10-14-6-中文"></a>
### 中文

Windows x64。本说明记录上次正式发布 [v2026.10.12.1](https://github.com/Loki-0228/MikuDanceStudio/releases/tag/v2026.10.12.1) 至本次发布的累计变化。

| 项目 | 上次正式发布 | 本次发布 |
| --- | --- | --- |
| 版本 | `v2026.10.12.1` | `v2026.10.14.6` |
| 提交 | `1b80cbf02b884890abb8939715fbbb258d7428e9` | 由版本标签 `v2026.10.14.6` 固定 |
| 状态 | 2026-10-12 已发布 | 2026-10-14 发布 |

对比范围覆盖上次正式发布之后的累计修改，以本次发布的最终行为为准。完整包与最小包来自同一固定源码，不包含工作区尚未提交的修改。

<a id="release-v2026-10-14-6-导出备份与编辑"></a>
#### 导出、备份与编辑

- 新增图片序列输出，可设置目录、文件名前缀、图片格式和帧范围。支持单帧图片输出已有的格式，序号位数随输出范围调整。
- 新增「自动保存设置…」，支持分钟间隔、备份目录、数量上限和覆盖旧备份。默认保存到程序同级的 `backups`，不覆盖手动保存的工程。发生崩溃时始终尝试备份，成功与否受崩溃状态及目录可写性等条件限制。
- `Ctrl+F` 强制粘贴支持不同表情间的数值粘贴。完整包增加时间轴强制粘贴按钮和最近工程菜单。
- 完整包增加骨骼九通道自由曲线，分别编辑位移、XYZ 角度和缩放，支持播放、定位及 PMM、VMD 扩展存储。曲线编辑器增加保持形状的双击插帧、控制点数值输入、斜率与权重拖动、关键帧跳转和侧栏调整。
- 修复固定轴骨骼载入 VMD 后的旋转角度及方向偏差。完整包修复非等比父级缩放后，子骨骼旋转产生额外形变的问题。
- 修复配件删除、切换或重载后的菜单状态和绘制分界。底部面板支持拖动调整宽度，松开鼠标后应用布局，修复部分控件越框、闪烁和残影。

<a id="release-v2026-10-14-6-预览与抗锯齿"></a>
#### 预览与抗锯齿

- 完整包新增视口渲染比例，支持自动匹配窗口、降低比例预览和超采样，不改变导出尺寸。低分辨率画面用最近邻放大，直接显示像素块。
- HUD、帧率及上下工具条按整个预览框布局，包含黑边区域，并在场景放大后按显示分辨率绘制。修复主窗、浮窗及设置窗口重绘遮挡 HUD 的问题。
- FPS 改为统计实际完成并成功提交的场景帧，排除省性能空循环、重复提交和离线录制采样。
- 抗锯齿菜单提供「MMD 原始（MSAA）」「FXAA」「TAA」和「SMAA」。SMAA 有 1x、T2x 各自的「标准」与「斜线与拐角增强」四档。直接点击应用上次选择，初始为「SMAA 1x（斜线与拐角增强）」；悬浮后向右移动可展开选项。旧工程的独立 MSAA 档位仍兼容读取。
- TAA 和 SMAA T2x 增加相机、骨骼及变形后几何的运动矢量重投影，并检查深度与历史有效性。当前色彩、历史色彩及深度使用对应的采样偏移；拒绝历史时消除当前偏移，地面网格也纳入运动捕获。
- 显卡支持顶点纹理读取时，TAA、SMAA T2x 将两帧变形后位置保留在 GPU，省去捕获结果每帧读回 CPU 再上传的过程。输入相同的材质共享捕获结果，已确认不透明的原生材质省去覆盖重绘。MME 和透明贴图保留覆盖检查；不支持该能力时使用兼容路径。SMAA 1x 行为不变。
- 仅有第 0 帧的静止图片会在同一时间和姿态下补足时间采样，不推进物理或增加输出图片。连续图片序列保留历史，SMAA T2x 首张绘制两次，后续每张一次。无法取得运动数据时回退到单帧抗锯齿。
- 抗锯齿开关、算法和上次 SMAA 档位随 PMM v2 扩展保存。加载工程不改写本机默认值，临时硬件回退不覆盖工程原选择。
- 修复视口比例菜单误触「打开场景」、最近工程菜单展开闪烁，以及导出后视口尺寸和 DPI 状态未正确恢复的问题。视口工具支持连续相对拖动，失去捕获时不再拉回光标。

<a id="release-v2026-10-14-6-阴影与兼容性"></a>
#### 阴影与兼容性

- 完整包自阴影恢复 MMD 9.32 的光源网格方向、投影变换及模式 1、2 的连续深度渐变，恢复线性深度采样。动态阴影精度共用同一范围设置，按实际投影密度分配纹理，保留高精度范围防裁切。
- 完整包新增「滤波加强」，默认关闭。开启后先对当帧阴影深度图进行两遍基础样条滤波，再交给原生材质、MME 和支持新接口的 MMV，避免外部 FX 漏读增强结果或宿主重复滤波。失败时回退到当帧原始深度图。
- 修复完整包在 MME 下关闭透明排序后，仍拆分不透明和透明材质的问题。关闭时按原模型材质顺序绘制。
- 修复部分材质启用 MME 后地面阴影异常加深的问题，使原生与 MME 读取相同的阴影颜色。原有阴影亮度、透明混合开关及 alpha 值保持不变。
- 修复完整包「最高」透明排序将预编译效果当作源码拼接的问题。透明剥层使用独立的完整效果资源，避免损坏字节码导致加载异常、透明层丢失或崩溃。
- 改善原版 MME 对中文路径和已有特效引用的只读访问兼容性。完整包向 MME 提交模型路径时，优先使用系统代码页能够准确表达的路径，改善同名 EMD 的查找。
- 完整包附带可选的 `MMSNvidiaAAFix.dll`，用于安装当前程序的 NVIDIA 双面绘制兼容设置。DLL 存在时，帮助菜单按检测结果提供安装或卸载入口；执行时请求 Windows 管理员授权。安装或卸载在下次启动 MMS 时生效。删除 DLL 会移除菜单入口，但不会撤销已安装的驱动设置。没有可用 NVIDIA 驱动时停止安装，AMD、Intel 不应用此设置。
- 新增设置和提示覆盖中文、日文、英文，并调整日英文案、按钮含义和窗口布局。

<a id="release-v2026-10-14-6-完整包与最小包"></a>
#### 完整包与最小包

| 本次新增功能或变化 | 完整包 | 最小包 |
| --- | --- | --- |
| 图片序列、周期备份、崩溃备份尝试 | 包含 | 包含 |
| 表情 `Ctrl+F` 强制粘贴、底部面板调宽 | 包含 | 包含 |
| 固定轴 VMD、地面阴影颜色、配件状态、导出视口等共享修复 | 包含 | 包含 |
| 最近工程、强制粘贴按钮、视口渲染比例 | 包含 | 不包含 |
| 九通道骨骼自由曲线、非等比缩放修复 | 包含 | 不包含骨骼缩放及新曲线模块 |
| 动态阴影、防裁切、滤波加强 | 包含 | 保留原版阴影方式及范围裁切 |
| AA 算法选择、TAA、SMAA、NVIDIA 补丁 DLL | 包含 | 保留原抗锯齿开关，不包含新增模块 |
| 最高透明排序效果加载修复 | 包含 | 不包含透明排序模块 |

完整包 ZIP 包含 `MikuMikuDance.exe`、`MMSNvidiaAAFix.dll` 和 `licenses/`；最小包 ZIP 包含程序和许可证。不附带模型、用户设置或 MME。主 ZIP 不包含 `MMV4MMS.dll`。

<a id="release-v2026-10-14-6-日本語"></a>
### 日本語

Windows x64 向け。前回の正式リリース [v2026.10.12.1](https://github.com/Loki-0228/MikuDanceStudio/releases/tag/v2026.10.12.1) から今回のリリースまでの累積変更を記載しています。

| 項目 | 前回の正式リリース | 今回のリリース |
| --- | --- | --- |
| バージョン | `v2026.10.12.1` | `v2026.10.14.6` |
| コミット | `1b80cbf02b884890abb8939715fbbb258d7428e9` | タグ `v2026.10.14.6` が指すコミット |
| 公開日 | 2026-10-12 | 2026-10-14 |

比較範囲は前回の正式リリース以降の変更全体で、今回の最終的な動作を基準としています。完全版と最小版は同じ固定済みソースからビルドされており、作業ディレクトリの未コミット変更は含みません。

<a id="release-v2026-10-14-6-出力バックアップ編集"></a>
#### 出力、バックアップ、編集

- 連番画像出力を追加。保存先、ファイル名の接頭辞、画像形式、フレーム範囲を設定できます。単一画像出力で利用できる既存の形式に対応し、連番の桁数は出力範囲に合わせて調整されます。
- 自動保存設定を追加。保存間隔（分）、バックアップ先、保存数の上限、古いバックアップの上書きを設定できます。既定の保存先は実行ファイルと同じ場所にある `backups` です。手動保存したプロジェクトは上書きしません。クラッシュ時には常にバックアップを試みますが、成功するかどうかはクラッシュの状態や保存先への書き込み可否などに依存します。
- `Ctrl+F` による強制貼り付けで、異なるモーフ間の数値貼り付けに対応。完全版にはタイムラインの強制貼り付けボタンと最近使ったプロジェクトのメニューを追加しました。
- 完全版に、ボーンの移動、XYZ 回転、拡大縮小を個別に編集する 9 チャンネルの自由曲線を追加。再生、フレーム移動、PMM・VMD 拡張による保存に対応します。曲線エディターには、形状を維持するダブルクリックでのキー挿入、制御点の数値入力、傾きとウェイトのドラッグ、キーフレームへの移動、サイドバーの調整を追加しました。
- 固定軸を持つボーンに VMD を読み込んだ際、回転角度や方向がずれる問題を修正。完全版では、親ボーンの非等方スケール下で子ボーンを回転させたときに余分な変形が生じる問題も修正しました。
- アクセサリの削除、切り替え、再読み込み後に、メニュー状態や描画の区切りが不正になる問題を修正。下部パネルの幅をドラッグで変更でき、マウスを離した時点でレイアウトを適用します。一部のコントロールのはみ出し、ちらつき、残像も修正しました。

<a id="release-v2026-10-14-6-プレビューとアンチエイリアス"></a>
#### プレビューとアンチエイリアス

- 完全版にビューポートの描画倍率を追加。ウィンドウサイズへの自動調整、低解像度プレビュー、スーパーサンプリングに対応し、出力サイズは変更しません。低解像度の画像は最近傍補間で拡大し、ピクセルをそのまま表示します。
- HUD、フレームレート表示、上下のツールバーを、黒帯を含むプレビュー領域全体に配置。シーンの拡大後に表示解像度で描画します。メインウィンドウ、フローティングビュー、設定ウィンドウの再描画で HUD が隠れる問題を修正しました。
- FPS は、描画が完了して正常に送出されたシーンフレームだけを数えるように変更。処理負荷を抑えるための空ループ、同じフレームの再送出、オフライン録画のサンプルは除外します。
- アンチエイリアスメニューに「MMD 標準（MSAA）」「FXAA」「TAA」「SMAA」を用意。SMAA は 1x と T2x のそれぞれに「標準」「斜線・コーナー強化」があり、計 4 種類です。直接クリックすると前回の選択を適用し、初期値は「SMAA 1x（斜線・コーナー強化）」です。項目にカーソルを合わせて右へ移動すると詳細を選択できます。旧プロジェクトの独立した MSAA 段階設定も引き続き読み込めます。
- TAA と SMAA T2x に、カメラ、ボーン、変形後ジオメトリのモーションベクトルによる再投影を追加し、深度と履歴の有効性を確認します。現在の色、履歴の色、深度にはそれぞれ対応するサンプルオフセットを使い、履歴を採用しない場合は現在のオフセットを取り除きます。地面グリッドもモーション取得の対象に含めました。
- 頂点テクスチャ参照に対応する GPU では、TAA と SMAA T2x の 2 フレーム分の変形後座標を GPU 上に保持。取得結果を毎フレーム CPU に読み戻して再転送する処理を省きます。入力が同じ材質では取得結果を共有し、不透明と確認できた標準材質ではカバレッジ取得用の追加描画を省略します。MME とアルファ付きテクスチャではカバレッジ確認を維持し、この GPU 機能に非対応の場合は互換処理を使います。SMAA 1x の動作は変更していません。
- フレーム 0 だけの静止画でも、時刻と姿勢を固定したまま時間方向のサンプルを補います。物理演算を進めたり、余分な画像を出力したりしません。連番画像では履歴を保持し、SMAA T2x は最初の画像だけ 2 回、以後は各画像を 1 回描画します。モーションデータを取得できない場合は、単一フレームのアンチエイリアスに切り替えます。
- アンチエイリアスの有効・無効、方式、前回の SMAA 選択を PMM v2 拡張に保存。プロジェクトを読み込んでも、この PC の既定値は書き換えません。ハードウェア制約による一時的な切り替えでも、プロジェクト本来の選択を上書きしません。
- ビューポート倍率メニューで「シーンを開く」が誤作動する問題、最近使ったプロジェクトのメニューを開く際のちらつき、出力後にビューポートのサイズや DPI 状態が正しく戻らない問題を修正。ビューポートツールは相対移動による連続ドラッグに対応し、マウスキャプチャを失ったときにカーソルを引き戻さなくなりました。

<a id="release-v2026-10-14-6-影と互換性"></a>
#### 影と互換性

- 完全版のセルフシャドウで、MMD 9.32 の光源グリッドの向き、投影変換、モード 1・2 の連続的な深度グラデーション、線形深度サンプリングを復元。動的な影解像度は共通の範囲設定を使い、実際の投影密度に応じてテクスチャを割り当てます。高解像度での範囲も保持し、影の欠けを防ぎます。
- 完全版に「フィルター強化」を追加。既定では無効です。有効にすると、そのフレームの影深度マップに 2 パスの B スプラインフィルターを適用してから、標準材質、MME、新インターフェース対応の MMV に渡します。外部 FX が強化後の結果を読み逃すことや、ホスト側で二重にフィルター処理することを防ぎます。失敗時は、そのフレームの元の深度マップに戻します。
- 完全版で透明ソートを無効にしても、MME 使用時に不透明材質と透明材質を分けていた問題を修正。無効時はモデル本来の材質順で描画します。
- 一部の材質で MME を有効にすると地面影が過度に濃くなる問題を修正。標準描画と MME が同じ影色を参照するようになりました。既存の影の明るさ、アルファブレンドの有効・無効、アルファ値は変更していません。
- 完全版の透明ソート「最高」で、プリコンパイル済みエフェクトをソースコードとして扱い、文字列を追記していた問題を修正。透明レイヤーのデプスピーリングには独立した完全なエフェクトリソースを使い、バイトコード破損による読み込み失敗、透明レイヤーの欠落、クラッシュを防ぎます。
- オリジナル MME による中国語パスや既存エフェクト参照の読み取り専用アクセスの互換性を改善。完全版から MME にモデルパスを渡す際は、システムのコードページで正確に表現できるパスを優先し、同名 EMD の検索を改善しました。
- 完全版には、現在のプログラムに NVIDIA の両面描画互換設定を適用するための任意 DLL `MMSNvidiaAAFix.dll` を同梱。DLL が存在する場合、検出結果に応じてヘルプメニューにインストールまたはアンインストールの項目を表示し、実行時に Windows の管理者権限を要求します。設定の適用・解除は次回の MMS 起動時に反映されます。DLL を削除するとメニュー項目は消えますが、適用済みのドライバー設定は解除されません。利用可能な NVIDIA ドライバーがない場合はインストールを中止し、AMD・Intel にはこの設定を適用しません。
- 新しい設定とメッセージを中国語、日本語、英語に対応させ、日本語・英語の文言、ボタン表記、ウィンドウレイアウトを調整しました。

<a id="release-v2026-10-14-6-完全版と最小版"></a>
#### 完全版と最小版

| 今回の追加機能・変更 | 完全版 | 最小版 |
| --- | --- | --- |
| 連番画像、定期バックアップ、クラッシュ時のバックアップ試行 | 対応 | 対応 |
| モーフの `Ctrl+F` 強制貼り付け、下部パネルの幅調整 | 対応 | 対応 |
| 固定軸 VMD、地面影の色、アクセサリ状態、出力後のビューポート復元などの共通修正 | 対応 | 対応 |
| 最近使ったプロジェクト、強制貼り付けボタン、ビューポート描画倍率 | 対応 | 非搭載 |
| ボーンの 9 チャンネル自由曲線、非等方スケール修正 | 対応 | ボーン拡大縮小と新しい曲線モジュールは非搭載 |
| 動的シャドウ、影の欠け防止、フィルター強化 | 対応 | 元の影描画方式と範囲クリッピングを維持 |
| AA 方式の選択、TAA、SMAA、NVIDIA 修正 DLL | 対応 | 元の AA 有効・無効設定を維持し、新モジュールは非搭載 |
| 透明ソート「最高」のエフェクト読み込み修正 | 対応 | 透明ソートモジュールは非搭載 |

完全版 ZIP には `MikuMikuDance.exe`、`MMSNvidiaAAFix.dll`、`licenses/` が含まれます。最小版 ZIP にはプログラムとライセンスが含まれます。モデル、ユーザー設定、MME は同梱していません。メイン ZIP に `MMV4MMS.dll` は含まれません。

<a id="record-release-notes-shared"></a>
## 共享阴影滤波修复

原始记录：`RELEASE_NOTES_20261014_SHARED.md`。其中的版本、日期和验证范围沿用原记录。

完整包开启“滤波加强”后，会先对当帧阴影深度图进行两遍样条滤波，再供原生材质、MME 和支持新接口的 MMV 读取。加载 EMD 指定的 FX 时，不再漏掉宿主的基础滤波；原生接收阶段会跳过重复滤波。

动态阴影精度、高精度投影防裁切和模式 1/2 的原版连续深度渐变保留。滤波尺度按实际粗图、细图尺寸计算；关闭增强仍使用原始深度图。分配或绘制失败时回退到当前帧原图，不复用上一帧结果。最小包不加入滤波加强。

MMV 同时传递实际纹理尺寸与预滤波状态。插件改动投影物后，重建的阴影也按同一基础样条规则处理。自定义 FX 的材质光照和作者自己的阴影函数保留，因此共享深度图不保证所有 FX 的最终颜色与原生完全一致。

低分辨率视口使用最近邻放大，取消双线性插值平滑，直接显示像素块。HUD、操作按钮和帧率文字在场景放大后按显示分辨率绘制，帧率仍来自场景帧计数；导出画面不加入编辑 HUD。文件菜单和最近工程子菜单减少重复重绘及不必要的条目重建，避免展开时触发已复现的菜单闪烁。

预滤波使用两张额外 R32F 纹理：8192 输出增加 512 MiB 显存，4096 输出增加 128 MiB。该版本采用已审核的基础共享样条，不包含此前接收阶段的屏幕导数与方向扩展；画面以本次对照图为准。

发布准备在本地完成。GitHub 对照基线为 v2026.10.12.1；提交差异、运行验证及包哈希随审核包保存，未经用户审核不上传。

<a id="release-v2026-10-13-1"></a>
## v2026.10.13.1 发布说明

原始记录：`RELEASE_NOTES_20261013.md`。其中的版本、日期和验证范围沿用原记录。

Windows x64，完整包与最小包来自同一份源码。此目录提供 GitHub Release 上传准备文件，尚未执行发布。

<a id="release-v2026-10-13-1-完整包"></a>
### 完整包

- 自阴影恢复 MMD 9.32 的光源网格方向、投影变换和模式 1、2 的深度渐变。保留高精度范围防裁切、动态分辨率和共用的阴影范围滑块。
- 在「动态精度」右侧新增「滤波加强」，默认关闭并记住设置。开启后按实际阴影贴图尺寸和屏幕足迹调整样条采样；关闭后使用原生线性深度滤波。支持原版 MME 已加载、使用内置材质的场景。外部 FX 自己的采样函数不由此开关替换。
- 命令菜单新增图片序列输出，可选择文件夹、前缀和单帧输出支持的格式。序号宽度根据默认结束帧数的十进制位数确定；首位大于等于 5 时增加一位，例如 500 帧使用四位。
- 帮助菜单新增「自动保存设置…」，对勾表示周期保存已启用。支持分钟间隔、目录、备份上限和覆盖旧备份。默认目录为 EXE 同级的 `backups`；备份名称含原名和时间戳，不覆盖手动保存文件。崩溃备份始终启用，但受崩溃现场可恢复性及目录容量限制。
- Ctrl+F 强制粘贴支持表情，并增加时间轴强制粘贴按钮。改进曲线编辑、关键帧密集区操作及底部面板布局。
- 修复非等比缩放后子骨骼旋转形变。增加最近打开的 PMM；底部面板拖动在松开鼠标后更新布局，视口拖动支持越过窗口边缘继续操作。

<a id="release-v2026-10-13-1-共享修复"></a>
### 共享修复

- 修复原版 MME 在中文路径中寻找同名 EMD、贴图和效果引用的兼容问题。
- 修复关闭透明排序时的材质绘制顺序及继承采样状态问题。
- 修复固定轴骨骼载入 VMD 后的旋转差异、配件菜单状态和绘制分界。

<a id="release-v2026-10-13-1-最小包"></a>
### 最小包

最小包使用原版阴影投影与 SM2、SM3 着色器，包括范围过小的原有裁切行为。它不包含「动态精度」「滤波加强」及完整包新增模块。格式限制见 [最小包说明](../reference/minimal-build.md)。

两个 ZIP 均只包含 `MikuMikuDance.exe` 和 `licenses/`。不包含模型、用户设置、MME 或 MMV 插件。连续轮廓和分层阴影拟合仍是后续实验，不在本次功能承诺中。

<a id="release-v2026-10-13-1-验证范围"></a>
### 验证范围

实拍对照使用 YYB Miku NT、原版 MME、无外部 FX、相同灯光和相机设置。覆盖两种自阴影模式、范围 9500 与 9900，并读取实际分配尺寸。另有高精度防裁切、三语开关布局与设置兼容性、最小包功能隔离检查。具体日志和校验值随准备目录的 `manifest.json` 记录。

增强滤波仍可能让薄阴影变软，低分辨率下已丢失的轮廓不能靠深度滤波完整恢复。未进行可靠性能基准，不宣称提升播放速度。

<a id="release-v2026-10-12-1"></a>
## v2026.10.12.1 release notes / 发布日志 / リリースノート

原始记录：`RELEASE_NOTES_20261012.md`。其中的版本、日期和验证范围沿用原记录。

[English](#release-v2026-10-12-1-english) | [中文](#release-v2026-10-12-1-中文) | [日本語](#release-v2026-10-12-1-日本語)

<a id="release-v2026-10-12-1-english"></a>
### English

`v2026.10.12.1` · 2026-10-12 · Windows x64. Changes since `v2026.10.11.7`.

<a id="release-v2026-10-12-1-fixes"></a>
#### Fixes

The full and minimal builds share these fixes:

- Corrected the vertical bone rotation handle's axis to match original MMD. From a side view, dragging the handle now rotates around the expected axis instead of mixing unintended X and Y rotation.
- Fixed textured PMD models turning gray or white with bone **Select** enabled when their shared Toon file table uses a different order. Shared textures and their colors now follow the referenced filename. The fix also covers fallback to shared Toon textures. The full build uses the same mapping for Toon data passed to MMV.
- Fixed repeated clicks getting stuck on one bone when rotation-only and translation-enabled bones overlap. Each click advances to the next eligible bone in model order and wraps to the first. This works with rapid clicks in the main and floating viewports. Shift selection and existing visibility and physics filters are preserved.

<a id="release-v2026-10-12-1-package-differences"></a>
#### Package differences

| Package | Included functionality |
| --- | --- |
| `full` | MMS additions, including bone scaling, the new curve editor, shadow reconstruction, optional transparency sorting, DX12, modern media, the MMV host, and additional baking. |
| `minimal` | Original MMD functionality and shared bug fixes, including original Bezier interpolation, rigid-body physics, WAV audio, AVI, and the native MME interface. The additions listed above are omitted. |

Both builds use the same source branch. The distribution layout remains `MikuMikuDance.exe` plus `licenses/`, without models, effects, user settings, or MME and MMV plugins. `MMV4MMS.dll` remains under separate testing. See the [README package comparison](../../README.md#downloads-and-package-differences).

<a id="release-v2026-10-12-1-中文"></a>
### 中文

`v2026.10.12.1` · 2026-10-12 · Windows x64。记录 `v2026.10.11.7` 之后的变化。

<a id="release-v2026-10-12-1-修复内容"></a>
#### 修复内容

完整包与最小包共用以下修复：

- 修正骨骼竖向旋转柄的轴向，使其与原版 MMD 一致。从模型侧面拖动时，按预期轴旋转，不再混入非预期的 X、Y 轴旋转。
- 修复共享 Toon 文件表顺序不同的 PMD 在开启骨骼「选择」后丢失贴图颜色、呈灰白色的问题。共享贴图及其颜色按引用的文件名匹配，也覆盖回退到共享 Toon 的情况。完整包提交给 MMV 的 Toon 数据同步采用此规则。
- 修复旋转骨骼与可移动骨骼重叠时，重复点击始终选中同一骨骼的问题。现在按模型内顺序选择下一个可选骨骼，末尾返回第一个。主视口和独立视口均支持快速连续点击，保留 Shift 选择、可见性和物理锁定规则。

<a id="release-v2026-10-12-1-完整包与最小包"></a>
#### 完整包与最小包

| 包 | 功能范围 |
| --- | --- |
| `full` 完整包 | 保留骨骼缩放、新曲线编辑器、阴影重构、可选透明排序、DX12、现代媒体、MMV 宿主和新增烘焙等 MMS 功能。 |
| `minimal` 最小包 | 保留 MMD 原有功能与共享缺陷修复，包括原有 Bezier 插值、刚体物理、WAV、AVI 和原生 MME 接口，不编入上述新增模块。 |

两档在同一源码分支维护。发布目录仍仅包含 `MikuMikuDance.exe` 与 `licenses/`，不附带模型、效果、用户配置、MME 或 MMV 插件。`MMV4MMS.dll` 仍单独测试。详细对照见 [README 的包说明](../../README.md#下载与版本区别)。

<a id="release-v2026-10-12-1-日本語"></a>
### 日本語

`v2026.10.12.1` · 2026-10-12 · Windows x64。`v2026.10.11.7` 以降の変更です。

<a id="release-v2026-10-12-1-修正内容"></a>
#### 修正内容

完全版と最小版に共通する修正です。

- ボーン縦回転ハンドルの回転軸を、オリジナル MMD と一致させました。モデルを横から見てドラッグしたとき、意図しない X・Y 軸の回転が混ざらず、想定した軸で回転します。
- 共有トゥーンのファイル表が通常と異なる順序の PMD で、ボーンの「選択」を有効にするとテクスチャの色が失われ、灰白色になる問題を修正しました。共有テクスチャとその色を、参照するファイル名に従って選びます。共有トゥーンへのフォールバックも対象です。完全版から MMV に渡すトゥーンデータにも同じ規則を適用します。
- 回転のみ可能なボーンと移動可能なボーンが重なったとき、繰り返しクリックしても同じボーンしか選べない問題を修正しました。モデル内の順序で次の選択可能なボーンに切り替わり、最後まで進むと最初に戻ります。メイン画面と別ウィンドウのどちらでも素早い連続クリックに対応し、Shift 選択、表示状態、物理による選択制限を維持します。

<a id="release-v2026-10-12-1-完全版と最小版"></a>
#### 完全版と最小版

| パッケージ | 機能範囲 |
| --- | --- |
| 完全版 `full` | ボーンスケール、新しい曲線エディタ、シャドウ処理の再構成、任意の透明ソート、DX12、各種メディア形式、MMV ホスト、追加のベイク機能を含みます。 |
| 最小版 `minimal` | 従来の Bezier 補間、剛体物理、WAV、AVI、ネイティブ MME インターフェースなど、MMD 本来の機能と共通の不具合修正を含みます。上記の追加機能は含みません。 |

両方を同じソースブランチで保守します。配布内容は引き続き `MikuMikuDance.exe` と `licenses/` のみです。モデル、エフェクト、ユーザー設定、MME・MMV プラグインは含みません。`MMV4MMS.dll` は別途テスト中です。詳細は [README のパッケージ比較](../../README.md#ダウンロードとパッケージの違い)を参照してください。

<a id="record-release-3"></a>
## v2026.10.11.3 构建与发布记录

原始记录：`RELEASE_20261011_3.md`。其中的版本、日期和验证范围沿用原记录。

发布源码固定为 `24691fd3da3ede8f47b64c751cde6612664249df`。从该提交导出独立源码，使用 VS 2022 构建 Windows x64 Release，开启 DX12，关闭 DIAG 和 PERF。其他会话随后修改的路线讨论文档未进入这个源码快照。生成的嵌入资源与已提交资源逐字节一致。

本版修复模型选择框点击前为空、模型默认语言和自动翻译选项沿用灰色状态，以及旧式表情、显示组和骨骼名称遇到无效编码时丢失整段文字的问题。翻译和回退只改变显示，不写回模型数据。前后对照、四种实际进程代码页与名称查询耗时见[名称修复验收](interface/languages.md#record-japanese-name-gaps)。反馈者未提供原模型，不能声称已复现其全部场景。

<a id="record-release-3-发布验证"></a>
### 发布验证

固定源码构建的界面语言、菜单语言、文件名编码三组回归全部通过。隐藏窗口原生与实际 MME 集成验证均通过，覆盖默认着色器、首个物体画面、设备重置、编译失败回退及阴影像素一致性。实际安装包独立启动、完成后台编译并正常退出，没有依赖外置 Toon 或未发布插件。本轮启动仅作运行检查，不新增性能结论。

正式程序已更新为 `build/Release/MikuMikuDance.exe`。替换前的 v2026.10.11.2 程序保存在 `.local/archives/japanese-name-gaps-20261011/MikuMikuDance-before-v2026.10.11.3.exe`，SHA-256 为 `1a34a7fde3712b58e7a0bcafa47dc79aa6b8bf4bfae66bf2faba67a9d42c130f`。本机配置与插件未替换。

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `MikuMikuDance.exe` | 4,728,320 字节 | `9d7483f1940184d85c886e2dcc392059c1f821e4bd400833c9844d159f5d0ab0` |
| `MikuDanceStudio-v2026.10.11.3-win64.zip` | 2,122,597 字节 | `ce5a0e13d25bc993e360bfe72e195f2a403fd3e3d0cad3e52636d8343fb8a876` |

ZIP 包含主程序、README、更新说明、项目与依赖许可；另提供独立 EXE 和 `SHA256SUMS.txt`。不包含测试程序、用户配置和测速缓存。

[v2026.10.11.3](https://github.com/Loki-0228/MikuDanceStudio/releases/tag/v2026.10.11.3) 已公开发布，Release ID 为 `409090137`。三个附件均通过服务端摘要与无认证公开下载的大小、SHA-256 核验；远端标签指向上述源码提交，GitHub latest 指向本版。结果保存在发布归档的 `verified.json`。

首次 EXE 上传在 180 秒后中断。确认服务端仅有无摘要的未完成附件后，只移除本次新草稿中的该半成品并重传；EXE、ZIP 和校验清单随后全部完成。失败与恢复记录均保留，未删除任何旧 Release、标签或完整附件。

旧正式 Release、标签及空草稿保留。先前会话的旧版删除请求被自动审批拒绝，本次不通过新脚本绕过该决定。

<a id="record-release-3-本地归档"></a>
### 本地归档

- 固定源码、构建日志、兼容测试和发布记录：`.local/artifacts/github-release/v2026.10.11.3/`。
- 独立构建：`.local/builds/names-release-20261011/`。
- 发布文件与来源清单：`.local/releases/v2026.10.11.3/`。
- 名称与菜单前后对照：`.local/artifacts/japanese-name-gaps-20261011/01/`。

上述目录保留为发布验收参考。

<a id="record-release-2"></a>
## v2026.10.11.2 构建与发布记录

原始记录：`RELEASE_20261011_2.md`。其中的版本、日期和验证范围沿用原记录。

发布源码为 `b3cfb98c29921b6d2f6189e1531a2189f0cc54b6`，构建前工作区干净。使用 VS 2022 构建 Windows x64 Release，开启 DX12，关闭 PERF 和 DIAG。

本版包含空场景默认着色器后台编译、按需音频初始化、程序目录内的运算器缓存、Toon 覆盖说明，以及此前的阴影和 MME 显示修复。功能与前后性能对照见[着色器启动验收](performance/startup.md#record-startup-deferred-effect)和[阴影及显示验收](rendering/shadows.md#record-shadow-preview-mme)。

<a id="record-release-2-构建和运行验证"></a>
### 构建和运行验证

从该提交导出独立源码后重新构建主程序、原生效果测试和 MME 效果测试。资源生成结果与源码中的资源一致。两项隐藏窗口测试均通过，覆盖附件导入、编译调度、设备重置、编译失败回退、着色器字节码和阴影像素一致性、实际 MME 初始化。

实际发布包在不附带 Toon 文件或 MMV 插件的情况下通过隐藏启动检查，完成后台着色器编译并正常退出。测试没有弹出或置顶窗口。

本地正式程序已更新为 `build/Release/MikuMikuDance.exe`。替换前程序保存在 `.local/archives/startup-release-20261011/MikuMikuDance-before-v2026.10.11.2.exe`，SHA-256 为 `3570089c327f1e9d6d847b2edf7814318305ac953e29eca6d46aab082e8d8636`。现有插件与用户配置保留。

<a id="record-release-2-正式程序启动时间"></a>
### 正式程序启动时间

按用户要求，对实际发布 EXE 再次进行新旧交替启动测量。旧版本源码为 `5a8cdefe0317b712d50cf0854ebf5f9b261cc0bd`，新版本为上述发布提交。两侧均采用本机 `build/Release/` 的运行环境，保留相同的 MMV4MMS 插件和用户配置，复制同一份有效运算器缓存，空场景无模型或音频。两侧均为 x64 Release，DX12 开启，PERF 和 DIAG 关闭。

窗口隐藏但未最小化，正常绘制和 Present，未清理系统文件缓存。通过进程内存日志测量，约每 20 ms 采样一次。顺序为旧、新、新、旧、旧、新，各测三次；采样时后台同时进行 GitHub 文件上传。

| 指标 | 第一次 | 第二次 | 第三次 | 平均 |
| --- | ---: | ---: | ---: | ---: |
| 旧版初始化完成 | 2.981 秒 | 2.936 秒 | 2.989 秒 | 2.969 秒 |
| 新版初始化完成 | 1.376 秒 | 1.418 秒 | 1.366 秒 | 1.387 秒 |
| 新版空场景首个 Present | 1.554 秒 | 1.603 秒 | 1.571 秒 | 1.576 秒 |
| 新版着色器就绪 | 3.141 秒 | 3.214 秒 | 3.161 秒 | 3.172 秒 |

初始化平均缩短 1.582 秒，约 53.3%。新版三次初始化波动范围为 1.366 至 1.418 秒。后台编译期间共进行 152 次窗口响应探测，100 ms 阈值下没有超时，最长约 29.0 ms。六次运行均命中测速缓存并正常退出。

这些数字表示隐藏窗口下的初始化、实际 Present 和效果对象接入时间，不是桌面视觉计时，也不是首次无缓存测速或播放 FPS。旧版没有首帧标记，因此只对两侧共同记录的初始化完成时间计算改善比例。完整着色器的就绪时间没有因延后编译而缩短。

原始运行目录、相同运行文件副本、进程日志、EXE 哈希和汇总保存在 `.local/artifacts/release-startup-20261011/`，汇总文件为 `summary.json`。

<a id="record-release-2-下载文件"></a>
### 下载文件

ZIP 包含主程序、README、更新说明、项目许可和依赖许可文本。未包含本机配置、测速缓存、测试程序或尚未发布的 MMV 插件。已有安装可使用单独的 EXE 更新。

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `MikuMikuDance.exe` | 4,726,272 字节 | `1a34a7fde3712b58e7a0bcafa47dc79aa6b8bf4bfae66bf2faba67a9d42c130f` |
| `MikuDanceStudio-v2026.10.11.2-win64.zip` | 2,121,237 字节 | `2db93b95f79d24d114bd416e5913e879a23cf1b2e65249c135c893cd67c777ff` |

发布附件另含 `SHA256SUMS.txt`。[正式发布页](https://github.com/Loki-0228/MikuDanceStudio/releases/tag/v2026.10.11.2) 已发布，Release ID 为 `409070024`。三个附件均通过 GitHub 服务端哈希和无认证公开下载的逐文件大小、SHA-256 核验；远端版本标签指向上述源码提交，GitHub latest 指向本版。原始结果为发布归档中的 `verified.json`。

上传曾遇到 TLS 中断和响应超时，后续读取服务端状态确认完整附件，未重复上传已完成文件。失败及恢复记录保存在同一发布归档。

旧正式版 `v2026.10.10.1` 及其标签保留。删除旧 Release、远端标签和本地标签的命令被自动审批拒绝，尚未执行。审批认为当前用户授权明确包含构建与发布新版，但未明确授权删除旧版。不能通过其他脚本或会话绕过该拒绝。旧的空草稿 `v2026.10.11.1` 也保持原状。

用户随后指定由「修复日区中文界面名称缺失」会话接手后续发布。本版基于已提交的 `b3cfb98`，不包含该会话正在处理的后续表情及显示组名称回退改动。

<a id="record-release-2-本地归档"></a>
### 本地归档

- 源码快照、构建日志、测试结果和发布记录：`.local/artifacts/github-release/v2026.10.11.2/`。
- 独立构建：`.local/builds/startup-release-20261011/`。
- 安装包与文件清单：`.local/releases/v2026.10.11.2/`。
- 被替换程序备份：`.local/archives/startup-release-20261011/`。

以上目录作为已验收发布参考保留，不纳入源码包。

<a id="record-native-media"></a>
## MMS 原生媒体导入与导出

原始记录：`NATIVE_MEDIA_20261007.md`。其中的版本、日期和验证范围沿用原记录。

<a id="record-native-media-导入"></a>
### 导入

MMS 使用 Windows Media Foundation 解码本机支持的音视频格式。MP4/MOV 视频逐帧解码到背景纹理，并支持时间轴定位。实际可用的编码取决于 Windows 安装的媒体组件。

<a id="record-native-media-导出"></a>
### 导出

透明 MOV 使用 Windows Imaging Component（WIC）逐帧编码 PNG，由 MMS 内置的 MOV 封装器写入文件。导出保留 Alpha 通道，支持自定义帧率和奇数宽高。当前 WIC PNG/MOV 路径不写入音轨，也暂不支持 3D Vision。

MP4/H.264 继续使用 Windows Media Foundation 编码器；其他 AVI 压缩器仍通过 DirectShow 提供。

透明录制将模型和配件渲染到独立的 A8R8G8B8 目标，隐藏背景图片与视频。回读时翻转图像方向并解除颜色预乘，再编码为 PNG。每帧同时显示到窗口后缓冲区，保持录制预览可见。
