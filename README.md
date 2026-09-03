# Qtrader UFT

Qtrader 是面向 WonderTrader UFT 语义实施的低延迟行情工程，并从
WonderTrader 集成代码中提取了可独立构建和验证的 C++17 核心。
旧版 HFT simulation 流水线及其订单簿、SPSC、共享内存和 HFT 专用基准
已经移除；默认工程只呈现 UFT 行情路径。

## 当前结构

```text
Level2 / ITCH 行情
  -> 协议解析与序号检查
  -> 定点整数 UFT 事件与预分配订单状态
  -> 整数合约 ID 与启动期预注册策略
  -> 策略回调
```

仓库中包含：

- MoldUDP64 拆包、序号检查和补包请求构造。
- Nasdaq/PSX ITCH 5.0 解析和订单生命周期处理。
- 固定容量 UFT Engine、整数合约 ID 和预注册策略回调。
- 定长订单状态表、成交去重和 Broken Trade 撤销语义。
- WonderTrader 集成代码与可重复基准脚本。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

开启 ASan/UBSan：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQTRADER_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## 当前有效性能结果

### PSX ITCH 5.0 → UFT 策略回调（2026-09-03，未隔离基线）

测试从同一批、同一顺序的 238,991 条 AAPL 原始 ITCH 5.0 消息开始。
两边分别执行自己的 ITCH 订单状态和 UFT 事件转换，最终到达语义等价的
UFT 策略回调。这个边界比较的是两套可独立运行的行情生产链路，不是只替换
一个分发函数的微基准。

测量条件：

- Ubuntu/Linux x86-64，Intel Core i9-13900HX，15 GiB 系统内存。
- 未使用 CPU 隔离；仅临时绑定 CPU 8，五轮迁核数均为 0。
- CPU governor 为 `powersave`，CPU 8 的 SMT 兄弟 CPU 9 未隔离。
- 开始使用 `LFENCE+RDTSC`，结束使用 `RDTSCP+LFENCE`。
- 延迟样本是一条完整输入消息的耗时，不是批次平均值。
- 正式五轮前先运行一次完整的独立进程级 warm-up；其输出只保留在 raw
  审计记录中，不进入 CSV 和统计。每个正式进程仍先做一次不计时正确性回放。
- 持续吞吐每条路径每轮累计有效计时至少 5 秒。
- 奇数轮按 WT→Q 执行，偶数轮按 Q→WT 执行。
- 两边的逐消息入口都经过 `noinline` ABI 边界并禁用 LTO。运行前的
  反汇编守卫确认 Qtrader 的合约范围检查、订阅槽读取和间接回调仍然存在。
- 正确性在不计时回放中使用覆盖订单号、定点价格、数量、方向、类型、
  买卖订单号和时间的顺序敏感指纹，并将回调数、解析器统计和日终订单
  状态逐项对账。指纹计算不在延迟或吞吐计时区间内。
- 延迟回调在入口立即留下时间戳，之后只做可观测的最小消费和计数；持续吞吐
  每一遍都核对策略回调数，Qtrader 还核对了生成、成功投递、回调交付和未投递数。

本节采用已经核验的 `uft-production-v3`。它强制先运行一个不入 CSV、
不参与统计且必须通过完整校验的预热进程；当前表格和结论全部来自该版本。

两条路径均产出 119,487 条委托和 120,057 条成交/撤单事件。
五轮全部为 `validation=PASS` 且 `throughput_delivery=PASS`。下表为
五轮结果的中位数。

| 原始 ITCH 消息→第一个策略回调入口 | 平均 | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| WonderTrader UFT | 120.88 ns | 117.81 ns | 169.07 ns | 221.15 ns |
| Qtrader UFT | 15.68 ns | 14.47 ns | 38.03 ns | 100.86 ns |

回调入口只对产生策略事件的 238,986 条输入取样。下表的整消息往返则覆盖
全部 238,991 条输入，包含最小策略回调执行到返回的时间。

| 原始 ITCH 消息整消息往返 | 平均 | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| WonderTrader UFT | 156.45 ns | 155.02 ns | 211.23 ns | 375.34 ns |
| Qtrader UFT | 35.13 ns | 32.24 ns | 60.77 ns | 123.60 ns |

| 5 秒以上墙钟持续吞吐 | 输入吞吐 | 输出/已验证回调吞吐 |
|---|---:|---:|
| WonderTrader UFT | 8.025 Mmsg/s | 8.043 Mevent/s |
| Qtrader UFT | 64.378 Mmsg/s | 64.527 Mevent/s |

按中位数计算，Qtrader 到第一个回调入口的平均延迟为 WonderTrader 的
约 1/7.71，p50 约为 1/8.14，p99 约为 1/4.45；持续输入吞吐约为
8.02 倍。

内存数字分开披露：Qtrader runner、内嵌 Engine、回调桥和适配器控制对象
的固定大小为 4,718,928 bytes（约 4.50 MiB）；本次 AAPL 容量配置下的
订单与成交记录表为 720,896 bytes（704 KiB）。两者合计 5,439,824 bytes
（约 5.19 MiB），不含分配器元数据。基准程序另外为语料
保留 48,750,000 bytes（46.49 MiB）虚拟容量，五组延迟样本容量合计
9,559,640 bytes（9.12 MiB）；五轮进程峰值 RSS 的中位数为 37,476 KiB。
保留容量不等于全部页面已驻留，因此不应与 RSS 直接相加。

ITCH `B` Broken Trade 现已转换为 `TradeBust`；本日 AAPL 语料中 `B=0`，
所以该分支由独立合成测试覆盖，不借助这份语料声称实盘验证。

这是“原始 ITCH 消息→策略回调”的同输入比较，不包含物理网卡收包、
策略计算、风控、交易网关和下单回报，因此不是完整交易系统延迟。

本轮没有设置 cpuset、`isolcpus/nohz_full/rcu_nocbs`，IRQ 也未迁移。
回调入口的五轮单次最大值中位数分别约为 9.31 微秒和 11.91 微秒，
说明未隔离环境仍有明显调度噪声。p50/p99 可作为未调优基线；max 和更
极端尾部不能当作隔离机器上的结果。

复现脚本：

```bash
RUNS=5 CPU=8 ./scripts/bench_wt_qtrader_uft_formal.sh
```

原始记录：

- `bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.csv`
- `bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.raw.txt`
- `bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.meta.txt`

### 已完成的 cpuset-only 对照

同一 `v3` 二进制、语料和五轮顺序还做过一次仅启用 cgroup v2 isolated
partition 的对照。CPU governor 仍为 `powersave`，没有启用启动期
`nohz_full/rcu_nocbs`，也没有把该结果描述成完整低噪声环境。

| 五轮中位数 | 未隔离 | cpuset-only | 变化 |
|---|---:|---:|---:|
| WonderTrader 回调入口平均延迟 | 120.88 ns | 121.74 ns | +0.71% |
| Qtrader UFT 回调入口平均延迟 | 15.68 ns | 16.85 ns | +7.45% |
| WonderTrader 输入吞吐 | 8.025 Mmsg/s | 8.030 Mmsg/s | +0.07% |
| Qtrader UFT 输入吞吐 | 64.378 Mmsg/s | 64.546 Mmsg/s | +0.26% |

这次 cpuset-only 对照没有证明延迟改善。对应原始记录位于
`bench/results/wt-qtrader-uft-formal-20260903-cpuset-isolated-warmup-v3-final.*`。

### 可选低噪声环境脚本（未执行、未产生性能结论）

项目只提供后续调优脚本，不再执行或发布这组后续实验结果。脚本固定针对
本机 CPU 8–9 这一对 SMT 兄弟和 CPU 8 的基准绑定方式；换机器前必须重新
检查 `lscpu -e`、NUMA 节点、CPU policy 和 IRQ 布局，不能直接照搬。

- `scripts/qtrader_cpuset_partition.sh`：动态创建、使用和恢复 cgroup v2
  isolated partition。
- `scripts/qtrader_low_noise_runtime.sh`：在上述 partition 基础上，把 CPU
  8–9 governor 临时改为 `performance`，停止活动的 `irqbalance`，尝试把
  可迁移 IRQ 移到 housekeeping CPU，并临时关闭 watchdog；`restore` 从
  `/run/qtrader-low-noise-runtime` 保存的状态恢复。
- `scripts/qtrader_boot_isolation.sh`：通过独立 GRUB drop-in 添加
  `nohz_full=8-9 rcu_nocbs=8-9 irqaffinity=0-7,10-31
  isolcpus=managed_irq,8-9 nowatchdog`。安装和移除后都必须重启 Linux 才会
  改变当前内核状态；它不修改 Windows 分区或 Windows 启动配置。

只查看状态不会改动系统：

```bash
./scripts/qtrader_cpuset_partition.sh status
./scripts/qtrader_low_noise_runtime.sh status
./scripts/qtrader_boot_isolation.sh status
```

如果用户自行决定启用，完整顺序为：

```bash
sudo ./scripts/qtrader_boot_isolation.sh install
sudo reboot

# 重启后
sudo ./scripts/qtrader_low_noise_runtime.sh setup
sudo ./scripts/qtrader_cpuset_partition.sh run \
  env BENCH_BIN=/持久目录/ParserITCHUftFormalBench \
  ./scripts/bench_wt_qtrader_uft_formal.sh
sudo ./scripts/qtrader_low_noise_runtime.sh restore
```

基准默认二进制路径是 `/tmp/ParserITCHUftFormalBench`，而 `/tmp` 可能在重启后
被清空。因此启动参数实验前必须重新构建，或者像上面一样用 `BENCH_BIN`
指定持久目录中的可执行文件。基准脚本会在开始前检查二进制、ITCH 语料和三份
WonderTrader 配置，并把 watchdog、governor、cpuset、`nohz_full` 和仍落在
CPU 8–9 上的 IRQ 状态写入新结果的 meta 文件。

恢复启动参数同样需要再次重启：

```bash
sudo ./scripts/qtrader_boot_isolation.sh remove
sudo reboot
```

运行 `setup` 后即使中途失败，也应先执行 `restore`，不要直接删除 `/run` 中
的状态文件。`nowatchdog` 和运行期 watchdog 设置会降低内核锁死检测能力，
仅适合用户明确接受该诊断能力取舍的短时专用环境。cgroup v2 partition、
`nohz_full`、RCU offload 和 managed IRQ 的语义应以 Linux 内核的
[CPU isolation 文档](https://docs.kernel.org/admin-guide/cpu-isolation.html)、
[内核参数文档](https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html)
和 [lockup watchdog 文档](https://docs.kernel.org/admin-guide/lockup-watchdogs.html)
为准。

## 目录结构

```text
include/qtrader/               可复用的低延迟组件
tests/                         UFT、ITCH、MoldUDP64 正确性与差分测试
tools/                         ITCH 检查、回放和 MoldUDP64 工具
bench/                         UDP 回环基准
scripts/                       UFT 评测与可选系统调优脚本
integration/wondertrader/      WonderTrader UFT/ITCH 集成快照
third_party/wondertrader/      行情二进制 ABI 定义
```

## 项目范围与局限

- 当前已有 MoldUDP64 拆包、序号缺口检测和补包请求构造，但仍是
  本机离线/回环重放；还没有实盘组播接入、真实交易柜台或实盘风控。
- 当前发布边界止于 UFT 策略行情回调，不包含策略决策、风控、下单和回报链路。
- 固定容量表必须按目标市场的活跃订单峰值在启动期配置；容量不足会被统计为错误，
  不会在热路径扩容。

WonderTrader 派生的数据结构与集成文件保留原 MIT 许可，见
`LICENSE.wondertrader`。
