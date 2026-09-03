# WonderTrader UFT 集成层

这里仅保存 Qtrader UFT 所需的 WonderTrader 集成文件。旧 HFT simulation、
HFT 延迟工具、订单簿、SPSC/共享内存实验和对应对象池实验已经删除。

## 文件对应关系

| Qtrader 内路径 | 覆盖到 WonderTrader 的路径 | 作用 |
|---|---|---|
| `ParserITCH/` | `src/ParserITCH/` | Nasdaq/PSX ITCH 5.0 转换、UFT 回放、冒烟测试和正式基准 |
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

## UFT 验证目标

开启 `-DQTRADER_BUILD_ITCH_UFT_SMOKE=ON` 会构建：

- `ParserITCHUftSmoke`：验证
  `ParserITCH -> ParserAdapter -> WtUftEngine -> UftStraContext` 回调链路；
- `ParserITCHUftBench`：用于 UFT 局部诊断，不作为 README 正式性能结论；
- `ParserITCHUftFormalBench`：从相同原始 ITCH 消息分别进入 WonderTrader
  UFT 和 Qtrader UFT，执行独立不计时语义审计、单事件延迟测量和至少
  5 秒墙钟持续吞吐验证。

正式结果只采用根目录 README 标明的 `uft-production-v3` 记录。该基准
不包含物理网卡收包、策略决策、风控、交易网关或下单回报。

## 使用原则

将 `ParserITCH/` 和顶层 CMake 变更按路径合并到匹配版本的完整
WonderTrader，再运行 WonderTrader 原有测试、Qtrader UFT 差分测试和
sanitizer。不要把该快照机械覆盖到不同 WonderTrader 版本。
