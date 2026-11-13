# MME 相对效果与偏好持续性

旧 MMEffect 将 GetModuleFileNameA(NULL) 所得目录用于展开路径；ABI 适配后该目录位于 DLL 内容缓存。ReadAssignments 现同时建立 EXE 目录及实际 MMEffect 模块目录到工程目录的去重映射。无法查询模块目录时保留原 EXE 映射。

本机真实 MME 0.37 x64 验证使用其实际 CreateFileA 导入表槽位读取缓存目录展开的 effects/relative.fx：该文件仅存在于 relative-project/effects，读取内容必须恰为 project-relative-effect。长英文与中文 EXE 名称两例均通过，并继续通过绘制、效果切换、删除配件和退出检查。证据：本地验收记录（未随本文发布）。此项证明实际导入接线及路径解析，不等于覆盖 MME 界面中的所有 EMM 工作流。

仍未完成：MMEffect.ini 当前位于包含旧 DLL 及 Adapter 内容哈希的目录；升级后新缓存不能自动继承旧偏好。真实 MMEffect 导入包含文件 API，未见 Windows Profile API；MME-RE 的 Initialize 证据 ev_78eda236c9fae95bb469f78eb672e92c5797c9a71b8777131c832284c2223ed2 显示其内部构造并解析 INI。因此不能仅拦截 Profile API。后续需以稳定插件身份承载偏好，覆盖宿主默认值读写和旧 MME 文件访问，并验证升级、并发及中文路径。旧缓存存在多份不同偏好时不得按时间盲选，应保留并报告冲突。

后续实现：偏好已移至独立的 settings/插件身份SHA256/MMEffect.ini。该哈希不含 DLL 内容，也不引用缓存版本域。MMEffect 模块的目标 INI 的 CreateFileA/W 与属性查询使用稳定文件，其他文件调用保留原路径与已有钩子。首次初始化只写自有 staging 文件并原子发布；移除宿主 Profile API 默认值写入口，避免绕过文件身份检查。配置打开拒绝重解析点和多硬链接；现有文件先校验再截断。按插件身份独占锁覆盖运行期，避免不同 DLL 缓存代次同时写同一偏好。

旧缓存没有插件身份标记，不自动猜归属。稳定 INI 缺失且存在旧缓存 INI 时，插件拒绝初始化并报告迁移目标；原文件保留，待用户确认后复制。此为有意保留的人工迁移边界，不能宣称所有旧配置均自动迁移。

验证：本地验收记录（未随本文发布）。中英文 EXE 各运行首次启动及适配器副本追加 PE overlay 后的第二次启动：两代 DLL 缓存保持同一偏好文件，实际 MMEffect 宽字符导入写入的标记被保留两次；渲染、相对效果与退出通过。英文例另验证占用 preferences.lock、无归属旧 INI、指向哨兵文件的 INI 硬链接，均拒绝启动旧插件，哨兵和旧内容不被改写。两配置产品构建通过；未新增 Win7/AMD 真机证据，也未覆盖所有 MME 版本或所有菜单保存方式。
