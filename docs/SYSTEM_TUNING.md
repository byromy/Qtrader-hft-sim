# 可选系统调优

[返回项目首页](../README.md) · [性能测试说明](BENCHMARKS.md)

这些脚本用于减少性能测试时的系统干扰，普通编译和功能测试不需要运行。
下面的组合调优还没有测过性能；已完成的 CPU 分区隔离对比见性能测试说明。
所有命令均在仓库根目录执行。脚本固定针对
测试机器的 CPU 8–9 这一对 SMT 兄弟和 CPU 8 的基准绑定方式；换机器前必须重新
检查 `lscpu -e`、NUMA 节点、CPU policy 和 IRQ 布局，不能直接照搬。

- `scripts/qtrader_cpuset_partition.sh`：动态创建、使用和恢复 cgroup v2
  isolated partition。
- `scripts/qtrader_low_noise_runtime.sh`：在 CPU 隔离分区基础上，把 CPU
  8–9 governor 临时改为 `performance`，停止活动的 `irqbalance`，尝试把
  允许迁移的硬件中断移到其他 CPU，并临时关闭 watchdog；`restore` 从
  `/run/qtrader-low-noise-runtime` 保存的状态恢复。
- `scripts/qtrader_boot_isolation.sh`：通过单独的 GRUB 配置文件添加
  `nohz_full=8-9 rcu_nocbs=8-9 irqaffinity=0-7,10-31
  isolcpus=managed_irq,8-9 nowatchdog`。安装和移除后都必须重启 Linux 才会
  改变当前内核状态；该脚本涉及 GRUB 启动配置，操作前应了解本机启动布局并具备恢复手段。

只查看状态不会改动系统：

```bash
./scripts/qtrader_cpuset_partition.sh status
./scripts/qtrader_low_noise_runtime.sh status
./scripts/qtrader_boot_isolation.sh status
```

确认 CPU 编号、了解下面的改动，并准备好测试程序和行情文件后，按以下顺序操作：

```bash
sudo ./scripts/qtrader_boot_isolation.sh install
sudo reboot

# 重启后
sudo ./scripts/qtrader_low_noise_runtime.sh setup
sudo ./scripts/qtrader_cpuset_partition.sh run \
  env BENCH_BIN=/absolute/path/ParserITCHUftFormalBench \
  ITCH_FILE=/absolute/path/20190730.PSX_ITCH_50 \
  ./scripts/bench_wt_qtrader_uft_formal.sh
sudo ./scripts/qtrader_low_noise_runtime.sh restore
```

上述两个 `/absolute/path/` 路径需要替换为本机的程序和行情文件路径。
测试程序默认路径是 `/tmp/ParserITCHUftFormalBench`，而 `/tmp` 可能在重启后
被清空。因此重启后测试前必须重新编译，或者像上面一样用 `BENCH_BIN`
指定持久目录中的可执行文件。测试脚本会在开始前检查程序、ITCH 文件和三份
WonderTrader 配置，并把 watchdog、governor、cpuset、`nohz_full` 和仍落在
CPU 8–9 上的 IRQ 状态写入新结果的 meta 文件。

恢复启动参数同样需要再次重启：

```bash
sudo ./scripts/qtrader_boot_isolation.sh remove
sudo reboot
```

运行 `setup` 后即使中途失败，也应先执行 `restore`，不要直接删除 `/run` 中
的状态文件。`nowatchdog` 和运行期 watchdog 设置会降低内核锁死检测能力，
关闭 watchdog 后，内核可能无法及时发现锁死；只应在专门用于短时测试的环境中使用。
cgroup v2 分区和这些启动参数的具体作用，请查阅 Linux 内核的
[CPU isolation 文档](https://docs.kernel.org/admin-guide/cpu-isolation.html)、
[内核参数文档](https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html)
和 [lockup watchdog 文档](https://docs.kernel.org/admin-guide/lockup-watchdogs.html)
为准。
