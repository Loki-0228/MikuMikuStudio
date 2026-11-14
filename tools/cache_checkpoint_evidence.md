# CACHE-02：检查点中断证据检查（Linux）

用于读取已经隔离保全的**单个或数个普通目录检查点文件**，区分文件短写、体长错误、SHA-256 不一致和检查点内部结构拒绝。不会选择活动版本，不会创建目录、修改缓存、自动恢复、删除或重试。没有实际故障现场时，可运行自带的隔离进程实验。

CACHE-02 仍是「长期维护失败及恢复闭环」开放项。已有 Windows 维护程序使用真实 Store 做 1,000 次提交和周期回收；另有检查点、回收硬退出与文件 API 故障专项。本工具不是那些专项的替代品，也没有重现历史上的长时维护失败。

## 构建和运行

在云端 checkout 根目录执行，产物留在新建的临时目录。需要 Linux、Python 3 和支持 C++17 的编译器；不需要额外的 Python 包。

```sh
work=$(mktemp -d)
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/io \
    tools/cache_checkpoint_evidence.cpp src/io/cache_catalog_codec.cpp \
    -o "$work/decode_checkpoint"
python -B tools/cache_checkpoint_evidence.py self-test --decoder "$work/decode_checkpoint" \
    > "$work/linux-results.json"
```

如有独立副本或只读挂载的故障证据，明确指定文件（不要在活动缓存根上运行）：

```sh
python -B tools/cache_checkpoint_evidence.py inspect --decoder "$work/decode_checkpoint" \
    /isolated-copy/commits/00000000000000000000000000000001.blob
```

工具只接收文件名形如 32 个小写十六进制字符加 `.blob` 的**普通检查点**文件，拒绝符号链接和多硬链接。它不扫描目录，不打开引用指向的文件，不读取 ADS。请传入上述步骤自己构建的本地解码器。JSON 中的卷号、文件索引和预期长度来自声明字段，并非在 Linux 核实了原 NTFS 身份。复制前需独立保留原文件的路径、时间、卷及文件身份、所有相关文件/意图日志的清单和哈希。可一起传几个文件分别检查，但前一个可解码不代表应回退到它。

- `short_header`：检查点文件没有写到 128 字节通用文件头。
- `invalid_checkpoint_envelope`：通用头、检查点种类、专用 owner/key、文件名生成号或声明长度不相符。
- `short_payload`／`trailing_bytes`：实际长度小于／大于通用头声明长度。
- `payload_sha256_mismatch`：长度匹配，但负载与通用头 SHA-256 不符。
- `checkpoint_codec_rejected`：通用头和 SHA-256 通过后，链接的实际生产 `DecodeCheckpoint` 拒绝内部数据。
- `checkpoint_codec_ok`：外层和编解码器均通过；**不等于 Store 接受、不等于检查点能恢复、更不授权删除**。文件状态是一次检查时的观察，不是整个目录的原子快照。

退出码：`0` 所有文件的候选字节可解码；`1` 至少一个明确不符合；`2` 无法读取／证据不确定。输出没有绝对缓存路径。超出读取上限的文件会记录大小而不计算整文件哈希。

## 实验证据的边界

自测用独立编写的合成字节，两次启动真实写入子进程，在 80 字节通用头、384 字节部分负载和完整文件处使用管道握手，保证实际发生逐段 `fsync`，不靠睡眠模拟慢盘。其中一次在部分负载刷完后被 SIGKILL；新的检查进程确认部分文件仍在且无法通过，而先前独立、完整的候选字节未变。另一进程继续写完新文件，实测可解码，也没有删除旧失败现场。另用同长篡改和 SHA-256 重算后的错误退休引用区分封装校验和生产 codec 拒绝。临时实验不会访问任何产品目录。

本实验无 Win32 Store、CNG、NTFS 共享锁、租约/删除权限、ADS、综合缓存源预算、只读盘、磁盘满、物理慢盘、磁盘掉电或系统崩溃覆盖。真实硬退出和 Linux 文件 `fsync` 也不是断电持久性证明。不根据这一结果关闭 CACHE-02。

真实复验需精确 Windows 构建及隔离外部目录：先保存已有失败证据并校验源码/EXE 指纹；对未创建的独立绝对路径明确运行 `cache_catalog_maintenance_tests --trace-store NEW_ROOT`，保留首次失败的 `DIAG thousand`、逻辑修订和 Win32 API trace；另在隔离副本上用测试自身的 `--inspect-existing` 做真正的生产固定读取，再用显式恢复专项核对重启后的完整头、原失败报告、重试行为，以及活跃数据和非测试所有文件是否保留。不得在原失败目录上为了出报告而恢复或清理。
