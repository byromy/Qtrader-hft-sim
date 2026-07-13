# WonderTrader 集成层

这里保存 Qtrader 对完整 WonderTrader 源码做过的低延迟改造文件。它们不是孤立的
第三方样例，而是从改造后的 WonderTrader 工程提取出的可审查快照。

## 文件对应关系

| Qtrader 内路径 | 覆盖到 WonderTrader 的路径 | 作用 |
|---|---|---|
| `ParserShm/` | `src/ParserShm/` | sequence SHM 消费、丢帧和重启检测 |
| `WtDtCore/ShmCaster.*` | `src/WtDtCore/` | sequence SHM 单生产者发布 |
| `Share/ShmRingProtocol.hpp` | `src/Share/` | 原系统使用的共享内存 ABI 与 reader/publish 实现 |
| `WtHftComponents/` | `src/WtHftComponents/` | reference 与 Dense 价格时间优先订单簿 |
| `WtLatencyHFT/` | `src/WtLatencyHFT/` | 原生 Tick 热路径预热、分阶段和尾延迟测量 |
| `WtLatencyHFTBench/` | `src/WtLatencyHFTBench/` | 原版/对象池优化版及其他组件基准的 CMake 入口 |
| `Includes/WTSObject.hpp` | `src/Includes/` | `WT_POOL_SINGLE_THREAD_FAST` 可选同线程无锁池 |
| `CMakeLists.txt` | `src/CMakeLists.txt` | 将新增基准目录纳入 WonderTrader 总构建 |

这些文件依赖完整 WonderTrader 的 `WtCore`、`WTSTools`、`WTSUtils`、配置和运行目录，
不能只在独立 Qtrader CMake 工程中链接。独立可运行的 SHM、SPSC、订单簿和 Level2
链路位于项目根目录的 `include/`、`src/`、`tests/` 与 `bench/`。

## 对象池开关的安全条件

`WT_POOL_SINGLE_THREAD_FAST` 只在专用基准目标 `WtLatencyHFTBenchPool` 中启用，
WonderTrader 默认目标仍保留自旋锁。只有对象的分配和最终释放都发生在同一线程时，
该优化才安全；跨线程释放可能访问创建线程已经销毁的 thread-local pool。

## 在完整 WonderTrader 中使用

将本目录中的文件按上表同步到同版本 WonderTrader，再使用原工程 CMake 构建。
对象池 A/B 可从 Qtrader 根目录运行：

```bash
WONDERTRADER_ROOT=../wondertrader-master \
RUNS=5 ./scripts/bench_wondertrader_pool.sh
```

不要把该集成快照机械应用到不同版本；应按文件逐项合并并重新运行 WonderTrader
原有单元测试、Qtrader 差分测试和 sanitizer。
