# 项目命名与兼容标识

## 程序与源码名称

项目名称为 MikuMikuStudio，简称 MMS。CMake 项目与主目标为 `MikuMikuStudio`，主体头文件目录及命名空间为 `mikumikustudio`。运行程序保留 `MikuMikuDance.exe` 文件名以适配原生插件。

对外标识与显示名称分开维护；窗口类名、导出名、CMake 选项、环境变量、配置目录和文件魔数不能作为普通文案批量替换。

## 线格式兼容

| 数据 | 写入标记 | 兼容读取 |
| --- | --- | --- |
| PMM 扩展 | `MMSXPMM2` | 同时识别旧 `MDSXPMM2` |
| VMD 完整名称 | `MMSNAME1` | 同时识别旧 `MDSNAME1` |
| VMD 骨骼缩放载荷 | `MMSXMMSX` | 兼容读取旧载荷标记 |
| VMD 缩放页脚 | `MMSXVMDX` | 兼容读取旧页脚标记 |

具体字节布局、大小及版本以[PMM 扩展参考](../../reference/pmm-extensions.md)和相应头文件为准。已经持久化的 `org.mikudancestudio.*` 等键属于兼容契约，不能因为项目更名而随意重写。

## 插件与配置

main 的菜单接口与 MMV C ABI 按各自导出和版本检查。外部插件源码或预设中的历史名称，须按其协议迁移规则处理；显示名称变更不能替代结构、路径和二进制兼容检查。

本地配置与缓存位置见[本地数据](../../reference/local-data-storage.md)。新增格式时明确新写入规则、旧数据读取及失败行为，避免只改写端导致旧文件无法恢复。
