# 界面语言、名称与路径

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-model-name-translation"></a>
<a id="record-model-name-translation-dictionary-and-name-identity"></a>
<a id="record-model-name-translation-verification-on-2026-10-10"></a>
<a id="record-model-name-translation-retained-local-outputs"></a>
<a id="record-ui-japanese-locale-names"></a>
<a id="record-ui-japanese-locale-names-verification-on-2026-10-10"></a>
<a id="record-ui-japanese-locale-names-local-outputs"></a>
<a id="record-japanese-name-gaps"></a>
<a id="record-japanese-name-gaps-修复"></a>
<a id="record-japanese-name-gaps-验证"></a>
<a id="record-accessory-message-encoding"></a>
<a id="record-accessory-message-encoding-验证结果"></a>
<a id="record-accessory-message-encoding-构建与证据"></a>
<a id="record-physics-editor-encoding"></a>
<a id="record-physics-editor-encoding-原因与修改"></a>
<a id="record-physics-editor-encoding-前后版本与构建"></a>
<a id="record-physics-editor-encoding-四种-acp-的真实窗口回归"></a>
<a id="record-physics-editor-encoding-窗口操作耗时"></a>
<a id="record-physics-editor-encoding-本地交付"></a>
<a id="record-menu-language-fix"></a>
<a id="record-menu-language-fix-验证"></a>
<a id="record-ja-en-ui-review"></a>
<a id="record-ja-en-ui-review-审查范围与修改"></a>
<a id="record-ja-en-ui-review-验证"></a>

## 三种界面语言

MMS 提供中文、英文和日文界面。固定文案、菜单、提示和窗口标题按界面语言选择；用户模型中的名称保留其真实文本，语言切换不改变文件数据的编码含义。

## 模型与材质名称

骨骼、表情和时间轴优先使用模型原始 Unicode 名称，避免从截断的窄字节镜像恢复。英文界面优先英文名，中文、日文优先主名称；首选为空时使用已有的另一种名称。材质双方都为空时由插件保留默认标签。

隐藏表情仍可参与时间轴、组合与求值，不因没有四栏面板而丢失。旧式分组和固定日文标签显式按 CP932 解码，附件名称遵循其既有编码标记。

## 路径与旧编码

Unicode 文件系统路径和文件内部固定字段是不同的限制。PMX 能载入完整名称，不等于旧 PMM、VMD 能无损保存同样文本。标准字段使用 CP932 或相应可还原编码，完整名称由 MMS 扩展保留，详见[文件兼容](../../guides/files.md)。

日区、中文、西文和 UTF-8 进程环境需要分别验证。不能仅因一台机器没有乱码就认定所有代码页已支持。

## 原生与插件界面

MME 菜单按命令 ID 翻译，保留状态与未知扩展项，不修改第三方 DLL 文件。MMV 的参数名称按自己的多语言描述回退；外部启动器可有固定语言，不随宿主自动变更。

## 绘制与回归

名称在行内垂直对齐并限制宽度，长文本不会覆盖帧列。字体与文字解码分别处理。物理编辑页关联骨骼和「无」选项遵循相同语言回退。

`ui_language_tests`、编码与菜单测试的用法见 [tests/README.md](../../../tests/README.md)。旧格式字段边界另见[原版限制](../../reference/legacy-mmd-limits.md)。
