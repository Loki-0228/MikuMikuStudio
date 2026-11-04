# MMHack 旧接口桥接与 MME 支持修复

工作区：`MMS-abi-dev`，实际分支 `codex/plugin-abi`。在已有未提交修改上追加本批次，没有切换分支或提交其他修改。

## 实现

- 从本地 MME-RE/src/mmhack 移植独立桥接模块，保留 22 个导出名称和序号，连接 MMEffect 的初始化、场景、绘制、设备和对象回调。
- 继续使用现有 37 个 Exp 导出和 MMSAdapter；宿主私有查询提供完整地址、宽路径和资源，避免从低 32 位 ID 反推指针，附件绑定使用宿主字段和骨骼范围校验。
- 同时跟踪 D3DXCreateEffectFromResourceA 与 D3DXCreateEffect，覆盖标准效果的预编译路径；设备清理时清空借用效果及纹理状态。
- 清单省略 hack 使用内置 MMHack；显式 hack 保留用户原版。内置桥接与 Adapter 一起放入内容缓存，发布脚本包含原创桥接 DLL。
- 桥接使用静态 CRT，避免现有系统依赖预加载器把 CRT API-set 名称误作磁盘文件。
- D3DX 头文件及许可随源码保存；构建无需访问相邻 MME-RE。

## 已验证

主程序、桥接 DLL、场景生命周期探针及新增测试均在 Release 构建成功。

6 项 CTest 通过：plugin_abi_contract，以及 5 项兼容测试：legacy_image_contract、legacy_adapter_renamed_host、legacy_adapter_missing_host_exports、legacy_mmhack_scene_router、legacy_mmhack_com_lifetime。

真实 MMEffect + 内置桥接报告：`build/verification/legacy-mme-z7mcxt9m/summary.json`。英文与中文改名宿主均通过：22 个导出名称/序号与标准效果捕获，实际 FX 像素变化、启停恢复、对象删除、退出、缓存升级、稳定 INI 和冲突拒绝；输入原版 DLL 哈希保持不变。

原版 MMHack 回归报告：`build/verification/legacy-mme-u2uwx337/summary.json`，英文改名宿主通过同一探针。

测试最初在受限执行环境退出 0xc0000142，改在桌面执行环境运行后通过。首次内置桥接真实集成因 CRT API-set 依赖预加载失败，改为静态 CRT 后上述验证通过。

## 验证范围

本批次使用简单附件 FX 及合成设备测试，未宣称全部第三方效果、所有 PMX 材质/绑定组合、多设备或 Windows 7 实机均已验证。没有启动或重启用户正在使用的宿主。
