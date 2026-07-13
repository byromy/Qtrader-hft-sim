# Qtrader

Qtrader 是基于 WonderTrader 实施的整体低延迟改造项目，并从改造后的仓库中提取为可独立构建的 C++17 工程。改造范围不只包括订单簿，还覆盖 WonderTrader 的行情共享内存分发、进程间通信协议、线程间队列、数据布局、内存管理，以及从  Level2回放到模拟下单的端到端链路：

```text
Level2 逐笔委托回放
  -> mmap 序列号广播环（SPMC）
  -> 缓存行隔离的 SPSC 环形队列
  -> 价格时间优先订单簿
  -> 示例策略
  -> 模拟市价单与成交回报
```

WonderTrader 原有接口和主要运行结构仍被保留：`ShmCaster` 与 `ParserShm` 在原有组件边界内增加序列号协议，旧版共享内存协议可以继续使用；原有`WtBtCore/MatchEngine` 也没有被删除。新写的价格时间优先订单簿用于更贴近逐笔Level2 的撮合实验，并为后续低延迟数据结构优化提供可验证的基线。

## 项目特点

- mmap SPMC 广播环：单生产者、多消费者独立游标、每槽独立序列号、覆盖丢帧统计和生产者运行代次重启检测。
- SPSC 环形队列：固定容量、无锁、生产者与消费者状态 `alignas(64)` 隔离，使用获取/释放内存序发布数据。
- 价格时间优先订单簿：支持限价、市价、撤单、改单和部分成交。
- 稠密订单簿（`DensePriceOrderBook`）：固定价格窗口、连续 64 字节对齐存储、位图最优价查找、固定容量订单索引和节点池；热路径不分配内存。
- WonderTrader 原生逐笔行情热路径基准：预热后分别统计行情分发、策略和模拟下单阶段的平均值及 p50/p99/p99.9。
- 对象池实验：保留原自旋锁路径，并以 `WT_POOL_SINGLE_THREAD_FAST` 构建专用对照目标，验证同线程分配释放时移除冗余锁的收益与约束。
- 正确性参考实现：以可读的 `std::map` 订单簿为可信基线，对 20 万次确定性随机操作逐事件差分校验。
- 评测：预热、吞吐、平均值/p50/p99/p99.9/最大值、流水线分阶段延迟、ASan/UBSan、perf 和 ftrace 脚本。

## 构建方法

```bash
cd Qtrader
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

开启 ASan/UBSan：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQTRADER_ENABLE_SANITIZERS=ON \
  -DQTRADER_ENABLE_AVX2=OFF
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## 性能评测

```bash
RUNS=10 SAMPLES=1000000 ./scripts/bench_spsc.sh
RUNS=5 ORDERS=200000 ./scripts/bench_dense_orderbook.sh
RUNS=3 EVENTS=200000 ./scripts/run_level2_pipeline.sh
```

传入 WonderTrader `WTSOrdDtlStruct` 连续二进制文件：

```bash
RUNS=3 ./scripts/run_level2_pipeline.sh /path/to/order_detail.bin
```

不传文件时使用确定性生成的逐笔委托流。输入二进制必须与 `third_party/wondertrader/WTSStruct.h` 的 ABI 一致。

## 评测结果

**Linux 裸机评测待补。** 以下结果是在 Ubuntu VMware 虚拟机中取得的版本间中位数，仅用于相同虚拟机环境下的相对比较

### WonderTrader 原生热路径

原生基准使用 WonderTrader HFT 引擎、策略上下文和模拟交易 API。每轮先运行 10 万
次预热行情，再测量 100 万次正式行情，并绑定到虚拟机 vCPU 2。下表使用默认自旋锁
对象池路径五次运行的中位数，用于展示原生链路的阶段测量能力：

| 原生热路径阶段 | 平均值中位数 | p50 中位数 | p99 中位数 |
|---|---:|---:|---:|
| 行情分发 | 780.00 ns | 850 ns | 1,380 ns |
| 策略决策 | 67.53 ns | 81 ns | 104 ns |
| 订单路径 | 984.39 ns | 1,166 ns | 1,598 ns |
| 模拟 API 调用 | 66.30 ns | 81 ns | 104 ns |
| 端到端 | 2,318.57 ns | 2,615 ns | 4,370 ns |

该表是原生路径的测量结果，不是与 Qtrader Level2 流水线之间的性能对照。

### SPSC 队列对照

SPSC 用于 Qtrader 新建 Level2 链路中的线程间移交，并未替换 WonderTrader 原生策略
分发。以下是五次虚拟机运行的代表性结果：

| 移交方式 | 饱和吞吐量 | 单条在途 p99 延迟 |
|---|---:|---:|
| 直接调用 | 不适用，接近计时下限 | 101 ns |
| SPSC 环形队列 | 7.04 M event/s | 525 ns |
| 互斥锁队列 | 2.72 M event/s | 7.98 us |
| 自旋锁队列 | 7.22 M event/s | 43.64 us |

自旋锁吞吐量偶尔接近 SPSC，但尾延迟明显更差；不需要线程解耦时，直接调用仍然最低。

### 补充实验：数据布局与 AVX2

该实验面向可批量计算的盘口特征，不是原生逐笔订单路径。对 1,048,576 份订单簿执行20 轮计算，十次运行中位数如下：

| 数据布局与计算方式 | 每条行情耗时中位数 | 相对速度 |
|---|---:|---:|
| 完整订单簿 AoS，标量 | 10.03 ns | 1.00x |
| 最优档热字段 SoA，标量 | 2.70 ns | 3.72x |
| 最优档热字段 SoA，AVX2 | 1.27 ns | 7.88x |

7.88x 是“完整 AoS 标量”到“紧凑 SoA 加 AVX2”的组合收益，其中 AVX2 相对 SoA标量约为 2.12x；不能表述为 WonderTrader 原生撮合或下单加速 7.88x。

### 订单簿与 Level2 链路

下表属于**改造项目内部的组件对照测试**。其中“订单簿基线”是为逐笔 Level2 链路新写的 `std::map` 价格时间优先订单簿，它侧重可读性和正确性，并作为稠密版本的可信正确性参考。它不代表整个项目建立在该订单簿之上，也不代表原版 WonderTrader。

原版 WonderTrader `WtBtCore/MatchEngine` 仍保留在基础系统中，主要服务于原有的逐笔行情驱动回测撮合。由于它与新链路的输入语义、撮合模型和计时边界不同，下表没有把二者强行放在同一微基准中比较。下表用于证明其中稠密订单簿相对于可验证基线的增益。

| 测试项目 | `std::map` 订单簿基线 | 稠密订单簿 | 变化 |
|---|---:|---:|---:|
| 提交订单吞吐量 | 3.19 M/s | 5.45 M/s | 1.71x |
| 撤单吞吐量 | 4.83 M/s | 6.61 M/s | 1.37x |
| 撮合吞吐量 | 2.33 M/s | 5.54 M/s | 2.38x |
| 改单吞吐量 | 3.88 M/s | 5.59 M/s | 1.44x |
| 全链路饱和吞吐量 | 2.11 M/s | 3.40 M/s | +61.6% |
| 订单簿与策略阶段 p99 | 1.21 us | 289 ns | -76% |
| 端到端 p99 | 8.74 us | 1.88 us | -78.4% |

## 目录结构

```text
include/qtrader/               可复用的低延迟组件
src/                           端到端行情回放链路
tests/                         正确性测试与差分测试
bench/                         微基准程序
scripts/                       构建、评测与跟踪脚本
integration/wondertrader/      完整 WonderTrader 低延迟集成快照
third_party/wondertrader/      行情二进制 ABI 定义
```

## 项目范围与局限

- 当前是离线回放与模拟下单，没有真实交易柜台、OMS 持久化、实盘风控或断线恢复。
- 稠密订单簿要求为交易品种配置合法最小变动价位和价格上下界；离线程序的最大公约数推断仅用于回放实验。
- mmap 环实现的是广播语义，不是工作窃取队列；慢消费者可能被覆盖，但会准确统计序列号缺口。
- AVX2 示例验证 SoA 批处理场景，不表示订单簿逐笔热路径天然适合 SIMD。

WonderTrader 派生的数据结构与集成文件保留原 MIT 许可，见 `LICENSE.wondertrader`。

原生逐笔行情热路径基准和对象池优化依赖完整 WonderTrader，独立 Qtrader CMake默认不构建它们，但相关改动源码已经包含在 `integration/wondertrader/`
