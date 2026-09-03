#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchOrderLifecycle.hpp"
#include "qtrader/NasdaqItchWtsAdapter.hpp"

#include <array>
#include <cassert>
#include <cstdint>

struct WtsTestSink {
  std::uint64_t order_details{0};
  std::uint64_t matches{0};
  std::uint64_t cancels{0};
  std::uint64_t trade_busts{0};

  void on_order_detail(const wtp::WTSOrdDtlStruct& order) {
    ++order_details;
    assert(order.index != 0);
    assert(order.otype == ODT_LimitPrice);
  }

  void on_transaction(const wtp::WTSTransStruct& transaction) {
    if (transaction.ttype == TT_Match) {
      ++matches;
    } else if (transaction.ttype == TT_Cancel) {
      ++cancels;
    } else if (transaction.ttype == qtrader::itch50::kWtsTradeBust) {
      ++trade_busts;
    } else {
      assert(false);
    }
  }
};

int main() {
  const std::uint8_t u16[] = {0x12, 0x34};
  const std::uint8_t u32[] = {0x12, 0x34, 0x56, 0x78};
  const std::uint8_t u48[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  const std::uint8_t u64[] = {0x01, 0x23, 0x45, 0x67,
                              0x89, 0xab, 0xcd, 0xef};

  assert(qtrader::itch50::read_u16(u16) == 0x1234U);
  assert(qtrader::itch50::read_u32(u32) == 0x12345678U);
  assert(qtrader::itch50::read_u48(u48) == 0x0123456789abULL);
  assert(qtrader::itch50::read_u64(u64) == 0x0123456789abcdefULL);
  assert(qtrader::itch50::expected_message_size('S') == 12);
  assert(qtrader::itch50::expected_message_size('A') == 36);
  assert(qtrader::itch50::expected_message_size('Q') == 40);
  assert(qtrader::itch50::expected_message_size('?') == 0);

  qtrader::itch50::OrderLifecycle lifecycle(16);
  std::array<std::uint8_t, 40> add{};
  add[0] = 'A';
  add[2] = 7;            // Stock locate = 7.
  add[18] = 42;          // Order reference = 42.
  add[19] = 'B';
  add[23] = 100;         // Shares = 100.
  add[35] = 25;          // Price = 25 Price(4) units.
  lifecycle.on_message(add.data(), 36);
  assert(lifecycle.active_orders() == 1);

  std::array<std::uint8_t, 23> cancel{};
  cancel[0] = 'X';
  cancel[2] = 7;
  cancel[18] = 42;
  cancel[22] = 40;
  lifecycle.on_message(cancel.data(), cancel.size());
  assert(lifecycle.active_orders() == 1);

  std::array<std::uint8_t, 31> execution{};
  execution[0] = 'E';
  execution[2] = 7;
  execution[18] = 42;
  execution[22] = 60;
  lifecycle.on_message(execution.data(), execution.size());
  assert(lifecycle.active_orders() == 0);
  assert(lifecycle.stats().adds == 1);
  assert(lifecycle.stats().cancels == 1);
  assert(lifecycle.stats().executions == 1);
  assert(lifecycle.stats().missing_order_ids == 0);
  assert(lifecycle.stats().over_reductions == 0);

  qtrader::itch50::PsxWtsAdapter adapter(20190730, 7, "TEST");
  WtsTestSink wts_sink;
  adapter.on_message(add.data(), 36, wts_sink);
  adapter.on_message(cancel.data(), cancel.size(), wts_sink);
  execution[30] = 9;  // Match number = 9.
  adapter.on_message(execution.data(), execution.size(), wts_sink);
  std::array<std::uint8_t, 19> broken{};
  broken[0] = 'B';
  broken[2] = 7;
  broken[18] = 9;
  adapter.on_message(broken.data(), broken.size(), wts_sink);
  adapter.on_message(broken.data(), broken.size(), wts_sink);
  assert(adapter.active_orders() == 0);
  assert(adapter.stats().errors == 0);
  assert(adapter.stats().order_details == 1);
  assert(adapter.stats().transactions == 3);
  assert(adapter.stats().trade_busts == 1);
  assert(adapter.stats().unknown_broken_trades == 1);
  assert(wts_sink.order_details == 1);
  assert(wts_sink.matches == 1);
  assert(wts_sink.cancels == 1);
  assert(wts_sink.trade_busts == 1);
  return 0;
}
