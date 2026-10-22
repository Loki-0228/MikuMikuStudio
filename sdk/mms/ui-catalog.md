# UI 只读目录参考

`mms.ui.catalog` 1.0 查询已注册元素、命令和属性，以及宿主最近发布的主窗口原生菜单与控件。C 头文件是 [ui_catalog.h](include/mms/ui_catalog.h)，C++17 包装是 [cpp/ui_catalog.hpp](include/mms/cpp/ui_catalog.hpp)。包装只保存同一张 C 服务表和原租约，不建立第二套对象。

## 范围与身份

| 范围 | 返回内容 |
| --- | --- |
| `MMS_UI_CATALOG_REGISTRY` | 原 `mms.ui` 注册树，包括六个宿主扩展根；命令和属性描述位于 `registrations`。 |
| `MMS_UI_CATALOG_MAIN_MENU` | 主窗口当前已物化的菜单行，包括子菜单、分隔符及未知标签的自绘行。尚未创建的动态菜单内容不在目录中。 |
| `MMS_UI_CATALOG_MAIN_CONTROLS` | 宿主登记且实际存在的主窗口子控件。来源是初始创建表和明确追加的控件，不能代表全部窗口或画布热点。 |

`scopes` 是非零位掩码。`requestedScopes` 记录请求，`availableScopes` 记录宿主已发布范围，`coverageFlags` 始终保留 `MMS_UI_CATALOG_INCOMPLETE_NATIVE`。本服务不承诺对话框、浮动窗口、时间轴与 HUD 自绘热点已覆盖。

注册元素复用原 `registerElement` 句柄。若该元素已经呈现为菜单，目录将原生元数据合并到同一条目，`scope` 同时包含注册树和菜单位，不重复创建对象。原生观察项的 `origin` 是 `MMS_UI_CATALOG_NATIVE_OBSERVATION`，owner 是 `mms`，ID 使用 `mms.catalog.*`。

原生控件的登记 ID 可跨控件重建保持，但呈现实例代次变化。尚无语义登记的菜单只提供本次运行的观察身份。菜单结构变化会退役旧观察；文字变化保留当前实例。`HWND`、`HMENU`、窗口命令数字和菜单位置均不是公共句柄。

## 已知字段与修订

`knownFields` 分别声明标签、启用、可见、选中、边界和呈现是否已知。未置位的字段不能按零值解释。例如，自绘菜单行的未知标签不等于空标签，只有注册描述的元素也不能推断为可见或已启用。

`target` 复用原 `MmsUiTargetRefV1`，包含元素句柄、呈现实例代次和工程代次。未呈现的注册元素实例为零，且没有 `PRESENTATION` 位。解析时仍须提交快照中的完整 `target`，包括这些零值。

`revision` 跟踪原生发布，`registryRevision` 跟踪原注册表，`projectEpoch` 来自原刷新系统。客户端比较前两者及工程代次，不能只比较其中一个。内容未变的发布不增加修订。已租用快照不会因后续更新、删除、同名对象重建或工程重置而改变。

这两个修订不构成整个快照的变化令牌。插件目标的边界和呈现来自原刷新缓存，缓存更新可先于下一次原生目录发布。修订相等不能推断几何或呈现实例未变；使用当前目标前仍须解析完整 `target`，并检查返回的边界和已知字段。

`bounds` 对原生控件使用主窗口客户区的 DIP 坐标。插件呈现的边界沿用原刷新目标发布的坐标空间。菜单行没有 `BOUNDS`，不能将其零边界用于定位菜单。

## C 服务表

| 接口 | 行为 |
| --- | --- |
| `getCapabilities(context, out)` | 返回支持范围与固定预算。 |
| `acquireSnapshot(context, scopes, out)` | 原子复制指定范围的元素及注册描述，返回一个不可变租约。 |
| `resolveElement(context, expected, out, lease)` | 验证完整目标引用，返回当前单条目及单独租约。旧实例、旧工程或失去原父对象身份的原生分支返回 `STALE`。 |
| `releaseSnapshot(context, lease)` | 释放快照或单条目租约，必须由原客户端在 UI 线程调用。停止期间仍可释放。 |

输出结构的 `h` 使用该结构的大小、major 1、minor 0。读取 `MmsSpan` 时使用其原 `stride` 和 `recordVersion`。失败不改调用方输出；越界范围、错误版本、错误线程和跨 owner 释放均返回错误，不产生部分结果。

元素、命令和属性合计最多 16,384 条；单次快照最多 8 MiB。每插件最多 16 个租约及 16 MiB，全局最多 128 个租约及 64 MiB。预算同时限制记录、复制文本和保留容量。超过预算不截断目录，调用方须释放已有租约或减少请求范围后重试。

## C++17 包装

`mms::sdk::CatalogServices::Query(host, services)` 协商原 C 表。`Capabilities`、`Acquire` 和 `Resolve` 使用同一客户端。`CatalogSnapshot` 和 `CatalogEntryLease` 只能移动，不能复制，也不能用移动赋值覆盖已有租约。

快照通过 `value()` 读取描述，通过 `Elements()`、`Registrations()` 取得原 span。`Element(index)` 和 `Registration(index)` 按原 stride 取得只读记录指针，越界返回空指针。单条目租约通过 `value()` 读取记录。

所有服务调用与 `Release()` 都在取得服务的 UI 线程执行。工作线程只能读取已经租用的不可变数据；发布工作时须自行保证租约直到读取结束。停止前显式释放并检查结果。`Release()` 失败保留原句柄，允许正确线程重试；析构只尽力释放，不能替代停止协调。

## 权限与副作用

目录查询只读取已经发布的数据，不读取输入框内容，不打开菜单，不执行命令、状态提供者、布局或刷新，也不求值场景。查询不改变播放头、工程数据、dirty、焦点、鼠标捕获和共同历史。

目录可读不授予刷新、移动、覆盖、聚焦或调用命令的权限。原生观察项不能作为可写布局目标或插件挂载父项。注册菜单和按钮继续遵守原 owner、能力及生命周期检查。全部本体业务命令尚未登记，普通 `mms.edit` 仍关闭。

本服务不保存设置或目录文件，也不新增系统或显卡要求。基础要求继续为 Win7 SP1 x64、CPU 路径及 AMD 兼容；本机测试不能代替相应真机验收。
