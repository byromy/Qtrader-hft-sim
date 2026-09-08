# 可选订单簿与通信组件

通用订单簿与通信组件，可独立构建。根工程中，src/Level2Pipeline.cpp
使用它们组成模拟链路；include/qtrader/UftOrderBookPipeline.hpp 则通过稠密订单簿
的 feed_* 接口维护行情深度。与 WonderTrader 的性能对比没有包含这些组件。

[项目首页](../README.md) · [架构与运行说明](../docs/ARCHITECTURE.md)

## 组件用途

| 组件 | 做什么 |
|---|---|
| `DensePriceOrderBook` | 用整数价格索引档位，同一档位的订单按先后顺序排列；提供行情更新和模拟撮合两类接口 |
| `PriceTimeOrderBook` | 参考撮合实现，用于检查价格优先、时间优先规则 |
| `SpscRing` | 在一个生产线程和一个消费线程之间传递数据 |
| `ShmRingProtocol` | 定义共享内存环的布局、发布和读取方式 |

## 代码来源

| 文件 | 来源 |
|---|---|
| include/qtrader/DensePriceOrderBook.hpp | Git blob 792656f9abfa878cb0564ffe8caa77dd0d3d2e67，包含 feed_add/reduce/delete/replace |
| tests/DenseOrderBookTest.cpp | Git blob 5a9a3fc7a87dcafd272801f03a166ae396dcf7d3，包含行情更新及索引反复删除测试 |
| PriceTimeOrderBook.hpp、SpscRing.hpp、ShmRingProtocol.hpp | 初始提交 8a26f2a 的 include/qtrader 同名文件 |
| PriceTimeOrderBookTest.cpp、ShmRingProtocolTest.cpp | 初始提交 8a26f2a 的 tests 同名文件 |

代码沿用 wthft、hftbench、wtshm 命名空间。SpscRing.hpp 补充了
其 uint64_t 所需的 cstdint 头文件；SpscRingTest.cpp 是新增的正确性测试。
共享内存协议依赖仓库根目录 third_party/wondertrader 中的 ABI 头文件。
派生代码沿用根目录 LICENSE.wondertrader。

## 单独编译和测试

在仓库根目录执行：

```bash
cmake -S components -B build-components -DCMAKE_BUILD_TYPE=Release
cmake --build build-components --parallel 4
ctest --test-dir build-components --output-on-failure
```

## 使用时注意

订单簿既包含模拟撮合操作，也包含行情更新接口。Level2Pipeline 使用模拟撮合接口；
UftOrderBookPipeline 使用 feed_*，完成 Nasdaq 新增、撤单、改单和执行减量映射，
成交撤销只撤销成交统计，不恢复挂单。它们是不同用途，不能混用。
SPSC 仅限单生产者、单消费者，容量 N 实际可存 N-1 项。
共享内存环支持多个读者和丢帧检测。但只在复制前后检查序号，不足以保证安全：写线程仍可能在读线程复制时覆盖同一块内存。
模拟程序会等待读线程读完再复用槽位。增加多个读者时，也必须等最慢的读者读完，或者另外实现读写互斥的槽位管理。
同线程内调用策略不需要经过队列。
