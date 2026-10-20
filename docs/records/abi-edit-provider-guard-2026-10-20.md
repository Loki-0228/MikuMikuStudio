# 编辑回调内的序列化器保护

插件的事务参与者在 `prepare`、`commit`、`cancel`、`getUndoState` 和 `releaseUndoState` 中不能注册或注销序列化器。这些回调执行时，`StorageStore` 本身不一定处于回调中；此前的宿主入口会接受替换，并通过序列化器的加载回调重新写入插件状态。

`Manager::Host` 现在检查 `EditStore::InCallback()`，返回 `MMS_E_BUSY`。注册失败不修改输出句柄，注销保留线程错误的优先级。两个事务阶段之间仍可替换序列化器，随后旧事务因恢复令牌变化而返回 `MMS_E_STALE` 并取消，不发布历史。

## 验收

真实测试插件分别从命令和 UI 队列进入上述五种回调，每次均尝试注销和注册，并检查恢复令牌、当前值、历史节点、发布通知及取消次数。成功操作还检查撤销和重做。另一个场景验证准备完成后合法替换序列化器及提交过期。

修复前，新增命令回归在 `manager_edit_checks.inc` 的结果断言处失败；修复后，包含队列场景的 `plugin_abi_contract` 通过 4470 项检查。日志位于 `build/verification/abi-property-stage-20261020/provider-red.log`、`provider-green.log` 和 `provider-queue.log`。

复验命令：

```powershell
& D:/CMake/bin/cmake.exe --build build/cmake --config Release --target plugin_abi_tests --parallel 4
& D:/CMake/bin/ctest.exe --test-dir build/cmake -C Release -R '^plugin_abi_contract$' --output-on-failure
```

本项不开放普通生产宿主的 `mms.edit`，也不代表声明式输入控件已实现。main 工作区及稳定版产物未修改。
