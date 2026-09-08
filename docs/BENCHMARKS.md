# 与 WonderTrader UFT 的性能对比

本页比较 Qtrader 与 WonderTrader UFT 从解析 ITCH 消息到策略回调的延迟和持续吞吐。
测试日期为 2026-09-03，测试程序版本为 `uft-production-v3`。
测试没有加入行情订单簿、共享内存、SPSC 和模拟撮合；后续代码修改尚未重新测量。
程序和输入文件的 SHA-256、编译器版本及机器配置保存在各组 `.meta.txt` 文件中。

[返回项目首页](../README.md) · [可选系统调优](SYSTEM_TUNING.md)

## 测试方法

测试从同一批、同一顺序的 238,991 条 AAPL 原始 ITCH 5.0 消息开始。
两个程序分别解析消息、维护订单状态，并把相同含义的委托和成交事件交给策略。
比较的是这段完整处理过程，不是单独测一个分发函数。

测试配置：

- Ubuntu/Linux x86-64，Intel Core i9-13900HX，15 GiB 系统内存。
- 未使用 CPU 隔离；仅临时绑定 CPU 8，五轮迁核数均为 0。
- CPU governor 为 `powersave`，CPU 8 的 SMT 兄弟 CPU 9 未隔离。
- 开始使用 `LFENCE+RDTSC`，结束使用 `RDTSCP+LFENCE`。
- 延迟样本是一条完整输入消息的耗时，不是批次平均值。
- 正式五轮前先运行一次完整的独立进程级 warm-up；其输出只保留在 raw
  输出文件中，不进入 CSV 和统计。每个正式进程仍先做一次不计时正确性回放。
- 持续吞吐每条路径每轮累计有效计时至少 5 秒。
- 奇数轮按 WT→Q 执行，偶数轮按 Q→WT 执行。
- 两边的逐消息处理函数均禁止内联，并禁用 LTO（链接时优化）。运行前检查反汇编，
  确认编译器没有把合约检查、订阅数组访问和间接回调优化掉。
- 正式计时前单独回放一遍，用顺序敏感的滚动哈希校验订单号、整数价格、数量、方向、类型、
  买卖订单号和时间，并核对回调数、解析器统计和日终订单状态。这些完整校验不计入延迟或吞吐。
- 延迟回调在入口立即留下时间戳，之后只做可观测的最小消费和计数；持续吞吐
  每一遍都核对策略回调数，Qtrader 还核对了生成、成功投递、回调交付和未投递数。

## 测试结果

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

## 内存占用

Qtrader runner、内嵌 Engine、回调桥和适配器控制对象
的固定大小为 4,718,928 bytes（约 4.50 MiB）；该实验的 AAPL 容量配置下的
订单与成交记录表为 720,896 bytes（704 KiB）。两者合计 5,439,824 bytes
（约 5.19 MiB），不含分配器元数据。基准程序另外为语料
保留 48,750,000 bytes（46.49 MiB）虚拟容量，五组延迟样本容量合计
9,559,640 bytes（9.12 MiB）；五轮进程峰值 RSS 的中位数为 37,476 KiB。
保留容量不等于全部页面已驻留，因此不应与 RSS 直接相加。

ITCH `B` Broken Trade 转换为 `TradeBust`；这天的 AAPL 数据中没有 B 消息，
所以成交撤销只用生成的测试数据检查，没有在这份真实数据上测到。

这是“原始 ITCH 消息→策略回调”的同输入比较，不包含物理网卡收包、
策略计算、风控、交易网关和下单回报，因此不是完整交易系统延迟。

未隔离组没有设置 cpuset、`isolcpus/nohz_full/rcu_nocbs`，IRQ 也未迁移。
回调入口的五轮单次最大值中位数分别约为 9.31 微秒和 11.91 微秒，
说明未隔离环境仍有明显调度噪声。这些结果包含系统调度的影响，尤其是最大延迟，不能当作 CPU 完全隔离后的表现。

## 如何运行性能测试

先按 [WonderTrader 集成说明](../integration/wondertrader/README.md) 编译 `ParserITCHUftFormalBench`，并准备有权使用的 PSX 行情文件。根目录的普通构建不会生成这个程序；测试脚本也不会自动下载数据或编译 WonderTrader。
以下命令在仓库根目录运行，CPU 编号需按实际机器调整。请把每次结果单独保存，不要覆盖已有测试记录：

```bash
BENCH_BIN=/absolute/path/ParserITCHUftFormalBench \
ITCH_FILE=/absolute/path/20190730.PSX_ITCH_50 \
RUNS=5 CPU=8 ./scripts/bench_wt_qtrader_uft_formal.sh
```

原始记录：

- [逐轮结果 CSV](../bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.csv)
- [原始程序输出](../bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.raw.txt)
- [环境与 SHA-256](../bench/results/wt-qtrader-uft-formal-20260903-unisolated-warmup-v3-final.meta.txt)

## 只开启 CPU 隔离后的对比

使用同一个测试程序、输入文件和五轮执行顺序，再开启 cgroup v2 的 CPU 隔离分区进行对比。
CPU governor 仍为 `powersave`，没有启用 `nohz_full/rcu_nocbs`。这组只改变 CPU 分区设置。

| 五轮中位数 | 未隔离 | cpuset-only | 变化 |
|---|---:|---:|---:|
| WonderTrader 回调入口平均延迟 | 120.88 ns | 121.74 ns | +0.71% |
| Qtrader UFT 回调入口平均延迟 | 15.68 ns | 16.85 ns | +7.45% |
| WonderTrader 输入吞吐 | 8.025 Mmsg/s | 8.030 Mmsg/s | +0.07% |
| Qtrader UFT 输入吞吐 | 64.378 Mmsg/s | 64.546 Mmsg/s | +0.26% |

这次只开启 CPU 分区隔离后，平均延迟没有变好。对应原始记录位于
[cpuset 逐轮结果](../bench/results/wt-qtrader-uft-formal-20260903-cpuset-isolated-warmup-v3-final.csv)、
[原始输出](../bench/results/wt-qtrader-uft-formal-20260903-cpuset-isolated-warmup-v3-final.raw.txt)
和 [环境记录](../bench/results/wt-qtrader-uft-formal-20260903-cpuset-isolated-warmup-v3-final.meta.txt)。
