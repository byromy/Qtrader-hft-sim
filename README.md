# Qtrader

C++17 低延迟行情引擎，重点是降低行情从解析到策略回调的延迟，提高消息处理吞吐量。
项目参考 WonderTrader UFT 的接口设计，使用整数合约 ID、预注册回调、预分配内存和同线程调用减少处理开销，并与 WonderTrader UFT 进行性能对比。

支持 Nasdaq/PSX ITCH 5.0 行情解析、订单簿维护和缺包恢复，另提供 mmap 共享内存、SPSC 队列与模拟撮合示例。订单更新和策略接收结果通过自动化测试检查。

## 架构与使用场景

Qtrader 沿用 WonderTrader UFT 的委托、成交和策略订阅方式，可以独立编译运行。

| 入口 | 策略得到什么 | 适用场景 |
|---|---|---|
| 事件分发：`UftMarketDataEngine` | 解析后的委托、成交/撤单事件 | 策略直接处理事件，不需要完整订单簿 |
| 行情簿：`UftOrderBookPipeline` | 更新完成后的只读订单簿及更新事件 | 读取买卖盘口、档位数量与订单顺序 |
| 模拟链路：`Level2Pipeline` | 示例策略及模拟撮合回报 | 验证共享内存、线程交接和撮合组件 |

策略可以直接接收行情事件，也可以在订单簿更新后读取盘口。两种方式都在同一线程内调用：

```text
ITCH 消息 → 解析与订单生命周期维护
                ├─ 委托/成交事件分发 → 策略
                └─ 订单簿更新事件 → 行情订单簿 → 策略

MoldUDP64 数据报 → 序号检查 / 有界乱序恢复 → 按序 ITCH 消息
                  （可选传输入口；收包及补包请求发送由调用方负责）
```

按策略需要选择其中一种接法。直接分发事件时仍会记录订单剩余量和成交信息，
但不额外维护每个价格档位的挂单队列。

独立模拟示例为 `生成行情/WTS 委托 → mmap → SPSC → 撮合簿 → 示例策略 → 模拟下单`。
**行情簿按交易所事件更新，撮合簿在本地生成成交，两者不混用。**
mmap 和 SPSC 不是同线程行情路径的必经阶段。

## 性能优化与功能

- **快速分发：**用整数合约 ID 索引订阅数组，启动时注册策略回调，同线程直接调用，避免不必要的线程交接。
- **内存预分配：**启动时分配订单和成交状态表，处理行情时不扩容；容量不足会报错并停止行情簿处理。
- **整数价格与订单簿：**用整数表示价格，按价格档位索引挂单，同价订单按先后顺序排列；支持新增、撤单、改单和成交扣量。
- **成交处理：**不可打印成交仍扣减挂单；成交统计按 match ID 去重；Nasdaq Broken Trade 撤销成交统计，不恢复挂单。
- **缺包恢复：**检查消息序号，缺包期间暂停策略回调，补齐后恢复；遇到无法处理的错误时停止回调。
- **测试：**与独立参考订单簿逐条比较订单、档位数量、买卖一价和订单顺序；完整数据校验放在性能计时之外。

各类行情消息的处理方式和补包流程见 [架构文档](docs/ARCHITECTURE.md)。

## 快速开始

独立工程需要 Linux、支持 C++17 的编译器、CMake ≥ 3.16 和线程库。
已验证环境为 Ubuntu x86-64 / GCC 13。根目录构建不需要完整 WonderTrader，
也不需要下载历史行情；所需 WTS 二进制结构头文件随仓库提供。

在仓库根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

默认配置包含 18 项测试。订单簿测试会生成小型 ITCH 文件，再用回放程序读取：

```bash
ctest --test-dir build -R uft_orderbook --output-on-failure
./build/qtrader_uft_orderbook_replay build/uft_book_fixture.itch 7 90 110 1 32 32
```

第二条命令使用前一条测试生成的文件，预期输出包括：

```text
replay=PASS messages=8 book_callbacks=8 active_orders=1 best_bid=0 best_ask=105 net_printed_volume=0
```

另可运行共享内存与撮合示例：

```bash
bash scripts/run_level2_pipeline.sh
```

该脚本对两种订单簿各运行 10,000 条生成事件。它用于检查链路能否跑通，输出的耗时不用于下面的性能对比。
读取真实 ITCH 文件所需的参数、文件要求和容量设置见 [运行说明](docs/ARCHITECTURE.md)。

## 测试

- 确定性测试：撤单、改单、不可打印成交、成交撤销、同价 FIFO，以及策略只能看到完整更新后的状态。
- 随机差分：固定种子下 10,000 次状态转换，与独立 `std::map` 模型逐事件比较。
- 恢复测试：首包丢失、乱序、重复包、补齐后恢复、窗口溢出及故障后停止回调。
- 组件测试：SPSC 顺序与容量、共享内存多读者与 fork/mmap、参考和稠密撮合簿。
- 测试结果：Release 和 ASan/UBSan 均 18/18 通过；ASan/UBSan 这次运行关闭了泄漏检测。

运行 sanitizer：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DQTRADER_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel 4
ctest --test-dir build-asan --output-on-failure
```

订单簿目前使用生成数据和随机测试数据检查，尚未用真实全天行情完整测试。

## 与 WonderTrader UFT 的性能对比

使用相同的 238,991 条 PSX AAPL 原始 ITCH 消息，比较 WonderTrader UFT 和 Qtrader
从解析行情到策略回调的延迟，以及持续处理消息的吞吐量。
测试包含解析、订单状态维护和事件分发，不包含行情订单簿、mmap、SPSC、网络收包和下单。

测试日期：2026-09-03。环境：i9-13900HX、Ubuntu x86-64、绑定 CPU 8、未隔离、`powersave`。
五轮统计取中位数，独立进程预热不入表，持续吞吐每轮每路径至少测量 5 秒。

| 指标 | WonderTrader UFT | Qtrader 事件分发 |
|---|---:|---:|
| 到首个策略回调入口的平均延迟 | 120.88 ns | 15.68 ns |
| 到首个策略回调入口的 p99.9 | 221.15 ns | 100.86 ns |
| 整条输入消息往返平均延迟 | 156.45 ns | 35.13 ns |
| 持续输入吞吐 | 8.025 Mmsg/s | 64.378 Mmsg/s |

这组测试中，Qtrader 到首个策略回调的平均延迟约为 WonderTrader 的 1/7.71，
持续输入吞吐约为其 8.02 倍。加入行情订单簿及后续修改后的代码尚未重新测量。

完整结果、计时方法、内存占用、CPU 隔离对比和运行步骤见
[性能测试说明](docs/BENCHMARKS.md)。运行对比测试需要另外编译完整 WonderTrader。
[系统调优脚本](docs/SYSTEM_TUNING.md) 可选，普通构建和功能测试不需要修改系统配置。

## 目录结构

| 位置 | 内容 |
|---|---|
| [include/qtrader](include/qtrader/) | ITCH/MoldUDP64、固定容量表、事件引擎、行情簿与恢复入口 |
| [components](components/README.md) | 稠密/参考订单簿、SPSC、共享内存协议和来源记录 |
| [tests](tests/) | 协议、事件语义、行情簿差分及恢复测试 |
| [tools](tools/) | ITCH 检查、转换和回放程序 |
| [src/Level2Pipeline.cpp](src/Level2Pipeline.cpp) | 单进程三线程的共享内存与模拟撮合示例 |
| [integration/wondertrader](integration/wondertrader/README.md) | 面向完整 WonderTrader 的集成快照与对比基准 |
| [bench/results](bench/results/) | 性能测试数据、程序输出和机器配置 |
| [scripts](scripts/) | 构建、实验及可选系统调优脚本 |

## 当前限制

- 行情簿入口目前处理一个合约、一个行情会话，使用单线程和一个策略回调；必须从确认没有挂单的会话起点开始回放。
- 尚未实现 GLIMPSE 快照导入、自动重连、实盘行情服务、交易柜台和实盘风控。
- 文件内自行编号无法发现源文件已经丢失的消息；网络缺包检测需要 MoldUDP64 等协议提供的序号。
- 模拟撮合只用于功能演示，不用于计算策略收益。

## 代码来源

部分组件复用了仓库早期实现，项目也包含 WonderTrader 派生代码。
具体来源见 [组件说明](components/README.md)，WonderTrader 派生文件保留其 [原始许可](LICENSE.wondertrader)。
