# 启动与资源准备

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-startup-processor-config"></a>
<a id="record-startup-processor-config-保存位置"></a>
<a id="record-startup-processor-config-启动耗时"></a>
<a id="record-startup-processor-config-与原版-926-的差异"></a>
<a id="record-startup-processor-config-修改验收"></a>
<a id="record-startup-processor-config-本地记录与版本"></a>
<a id="record-startup-deferred-effect"></a>
<a id="record-startup-deferred-effect-启动对照"></a>
<a id="record-startup-deferred-effect-正确性验收"></a>
<a id="record-startup-deferred-effect-版本与原始记录"></a>
<a id="record-startup-lazy-audio"></a>
<a id="record-startup-lazy-audio-功能验收"></a>
<a id="record-startup-lazy-audio-启动对照"></a>
<a id="record-startup-lazy-audio-本地版本"></a>
<a id="record-pmm-startup-calibration"></a>
<a id="record-pmm-startup-calibration-行为"></a>
<a id="record-pmm-startup-calibration-测量"></a>
<a id="record-pmm-startup-calibration-验证"></a>
<a id="record-pmm-startup-calibration-构建与记录"></a>
<a id="record-pmm-load-baseline-20261020"></a>
<a id="record-pmm-buffer-20261020"></a>
<a id="record-mme-cache-rejected-20261020"></a>
<a id="record-model-parallel-read-20261020"></a>
<a id="record-mme-cache-accepted-20261020"></a>
<a id="record-pmx-preparation-20261020"></a>
<a id="record-reference-d3dx-20261020"></a>
<a id="record-mme-cache-named-layout-20261020"></a>
<a id="record-resource-reuse-20261020"></a>
<a id="record-runtime-cache-20261021"></a>
<a id="record-parallel-prepare-20261021"></a>
<a id="record-cache-management-20261021"></a>
<a id="record-mme-cache-stack-fix-20261021"></a>

## 初始化顺序

启动先显示可交互的基础窗口，默认效果按需要后台准备；时间轴音频按使用情况初始化。资源是否可用和界面是否显示是不同阶段，不能只测窗口出现就报告工程加载完成。

## PMM 与模型读取

PMM 读取使用有界缓冲，模型资源按受控顺序交接。PMX 内存几何缓存以文件及布局身份复用解析结果，不在模型旁生成缓存文件。

符合条件的大型 PMX 在冷几何路径并行展开顶点：加载线程按原顺序读取，最多两个工作线程处理临时批次，不在工作线程读取文件、访问 D3D9 或共享模型缓存。返回前等待任务结束；SDEF 继承相同浮点设置，避免改变结果。小模型与缓存命中保留各自路径。

## MME 编译结果

编译缓存保存字节码，不共享本应独立的效果对象或可变参数。键包含实际效果内容、include、编译选项及适用的 D3DX 身份。损坏、过期或不支持的条目回到原编译入口，不能将缓存命中视为跨任意 D3DX 版本兼容。

缓存文件位于程序旁 `Cache/MME/<效果名称>/`。路径、文件哈希与 DLL 身份解析的大缓冲使用堆内存，避免深层 MME 回调耗尽栈。极长安装路径可能使磁盘写入失败，程序保留内存或原入口回退。

## 纹理解码与准备

纹理准备缓存位于 `Cache/Textures/<纹理名称>/`。源文件身份、格式及加载条件参与有效性判断，保留实际像素与 mipmap 语义。已有 GPU 纹理对象复用不等于另存一份常驻 CPU 像素。

冷 PMX 纹理准备合并重复绝对路径，排除已有 GPU 纹理。符合支持范围的 PNG、TGA 可在有限工作线程解码内存数据；文件读取、预算及 D3D9 创建仍由对应加载路径控制。缓存命中直接使用准备结果，不重复启动解码。临时数据随当前载入结束释放。

## 运行时缓存

未变化模型的顶点与阴影边界仅存在于内存，独立于磁盘准备缓存。骨骼、表情、几何编辑、缓冲更换、重载和设备重置使对应结果失效。边界复用只省扫描，不省阴影、特效或 Present。

## 缓存管理与仅内存模式

「帮助 → 缓存管理」统一控制 MME 和纹理磁盘缓存，默认启用缓存及自动清理，共用 4 GiB 容量并优先保留最近 10 个工程。固定工程、正在使用文件及共享归属分别保护，完整规则见[本地数据与缓存](../../reference/local-data-storage.md)。

`MMS_CACHE_MEMORY_ONLY=1` 禁止 MME 与纹理准备缓存的磁盘读写，不删除已有缓存，不改变设置、备份或导出文件。MME 字节码仍可进程内复用，退出后下一进程需重新准备；运行时顶点与阴影缓存不受这个磁盘开关影响。

## 验证与测量

分别测冷启动、磁盘缓存命中、同进程重开及首帧可用时间。确认相同插件、工程、设备和实际输出，不用诊断计时或少量样本推算播放 FPS。回归与小栈检查见 [tests/README.md](../../../tests/README.md)，方法见[性能测量](benchmarks.md)。
