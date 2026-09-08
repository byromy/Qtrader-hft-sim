# 在 WonderTrader 中运行 ITCH 适配器和性能测试

本目录用于在完整 WonderTrader 中运行 ITCH 适配器和 UFT 性能对比，
不是独立 Qtrader 工程的必需依赖。订单簿和共享内存模拟器属于根工程，
不需要复制到 WonderTrader，也不参与这里的性能测试。

[项目首页](../../README.md) · [性能结果与运行步骤](../../docs/BENCHMARKS.md)

## 文件对应关系

| Qtrader 内路径 | 合并到 WonderTrader 的路径 | 作用 |
|---|---|---|
| `ParserITCH/` | `src/ParserITCH/` | Nasdaq/PSX ITCH 5.0 转换、UFT 回放、基本功能测试和性能测试 |
| `CMakeLists.txt` | `src/CMakeLists.txt` | 在指定 `QTRADER_ROOT` 时加入 `ParserITCH` |

这些文件依赖完整 WonderTrader 的 `WtUftCore`、`WTSTools`、`WTSUtils`、
配置和运行目录，不能只在独立 Qtrader CMake 工程中链接。

`ParserITCH` 还依赖 Qtrader 的协议和 UFT 适配头文件。配置完整
WonderTrader 时需传入：

```bash
-DQTRADER_ROOT=/absolute/path/to/Qtrader
```

运行配置中的 parser 参数为 `path`、`symbol`、`date` 和可选 `gpsize`。
`ParserITCH/config/` 只包含 PSX 验证所需的最小交易时段、品种和 AAPL
合约元数据，不是完整 Nasdaq 合约库。

## 可编译的测试程序

开启 `-DQTRADER_BUILD_ITCH_UFT_SMOKE=ON` 会构建：

- `ParserITCHUftSmoke`：验证
  `ParserITCH -> ParserAdapter -> WtUftEngine -> UftStraContext` 回调链路；
- `ParserITCHUftBench`：用于测量 UFT 局部处理耗时，README 的性能表不使用它的结果；
- `ParserITCHUftFormalBench`：从相同原始 ITCH 消息分别进入 WonderTrader
  UFT 和 Qtrader UFT，先单独回放并校验事件，再测单条消息延迟，以及每条路径至少 5 秒的持续吞吐。

性能表使用 `uft-production-v3` 测试程序的结果，详见性能测试说明。测试
不包含物理网卡收包、策略决策、风控、交易网关或下单回报。

## 合并代码时注意

将 `ParserITCH/` 和顶层 CMake 变更按路径合并到匹配版本的完整
WonderTrader，再运行 WonderTrader 原有测试、Qtrader UFT 差分测试和
sanitizer。不同 WonderTrader 版本可能有接口差异，不要直接覆盖已有文件。
