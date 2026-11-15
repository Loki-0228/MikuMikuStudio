# 现代软体构建使用的 Bullet 2.75

此 recipe 以 `../bullet275/conanfile.py` 为基础，只额外清零 `btSparseSdf::Hash` 的完整键对象。64 位结构体在三个 `int` 与指针之间含有填充字节；原实现把未初始化的填充一起散列，导致同一形状与网格位置生成不同键，重复计算距离场并持续分配缓存。

保持距离场采样、插值、接触参数和求解顺序不变。Win7 的原 recipe 与 `profiles/win7-x64.lock` 保留；现代构建使用 `profiles/modern-x64.lock` 和独立 `build/conan-modern` 依赖目录。

先用 `tools/check_win7_bullet_source.py` 验证官方 `bullet-2.75.zip` 并解包至一个新的 staging 目录，再用本目录的 `conanfile.py` 替换 staging 中的 recipe。不要修改已有 Conan 包的头文件或库。

```powershell
python tools/check_win7_bullet_source.py <bullet-2.75.zip> --stage-recipe <新的绝对staging目录>
Copy-Item recipes/bullet275-softbody/conanfile.py <staging目录>/conanfile.py
conan create <staging目录> --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 -c tools.build:jobs=1
conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 --lockfile profiles/modern-x64.lock --output-folder build/conan-modern --build=missing
cmake --preset modern-x64
cmake --build --preset modern-x64-release --target MikuMikuStudio pmx21_tests cloth_compute_tests
```

现代 `pmx21_tests` 验证实际 Bullet 接触查询的缓存唯一性。`--model <PMX绝对路径> [--gpu|--auto]` 只读加载指定模型，在自有隐藏窗口中报告初始化、物理求解、碰撞类型耗时和缓存条目数；无后缀时使用 CPU。此入口不修改用户模型或设置，不承诺与完整界面的帧率相同。
