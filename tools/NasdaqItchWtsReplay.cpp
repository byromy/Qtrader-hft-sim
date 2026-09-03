#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/NasdaqItchWtsAdapter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

std::string_view trimmed_symbol(const std::uint8_t* bytes) {
  std::size_t length = 8;
  while (length != 0 && bytes[length - 1] == ' ') {
    --length;
  }
  return {reinterpret_cast<const char*>(bytes), length};
}

struct AuditSink {
  std::uint64_t order_details{0};
  std::uint64_t transactions{0};
  std::uint64_t cancel_transactions{0};
  std::uint64_t match_transactions{0};
  std::uint64_t trade_bust_transactions{0};
  std::uint64_t volume{0};
  std::uint64_t checksum{0};
  std::uint64_t errors{0};

  void on_order_detail(const wtp::WTSOrdDtlStruct& order) {
    ++order_details;
    if (std::strcmp(order.exchg, "PSX") != 0 || order.code[0] == '\0' ||
        order.trading_date == 0 || order.action_date == 0 ||
        order.action_time > 23'59'59'999U || order.index == 0 ||
        order.price <= 0.0 || order.volume == 0 ||
        (order.side != BDT_Buy && order.side != BDT_Sell) ||
        order.otype != ODT_LimitPrice) {
      ++errors;
    }
    checksum ^= order.index + order.volume + order.action_time;
  }

  void on_transaction(const wtp::WTSTransStruct& transaction) {
    ++transactions;
    if (transaction.ttype == TT_Cancel) {
      ++cancel_transactions;
    } else if (transaction.ttype == TT_Match) {
      ++match_transactions;
    } else if (transaction.ttype == qtrader::itch50::kWtsTradeBust) {
      ++trade_bust_transactions;
    } else {
      ++errors;
    }
    if (std::strcmp(transaction.exchg, "PSX") != 0 ||
        transaction.code[0] == '\0' || transaction.trading_date == 0 ||
        transaction.action_date == 0 || transaction.action_time > 23'59'59'999U ||
        transaction.index == 0 || transaction.price <= 0.0 ||
        transaction.volume == 0) {
      ++errors;
    }
    volume += transaction.volume;
    checksum ^= static_cast<std::uint64_t>(transaction.index) +
                transaction.volume + transaction.action_time;
  }
};

int run(const char* path, const std::string_view symbol,
        const std::uint32_t trading_date) {
  std::unique_ptr<qtrader::itch50::PsxWtsAdapter> adapter;
  AuditSink sink;
  std::uint16_t locator = 0;
  std::uint64_t target_messages = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto scan = qtrader::itch50::scan_historical_file(
      path, [&](const std::uint8_t* message, const std::size_t size) {
        if (message[0] == 'R' &&
            size == qtrader::itch50::expected_message_size('R') &&
            trimmed_symbol(message + 11) == symbol) {
          locator = qtrader::itch50::read_u16(message + 1);
          adapter = std::make_unique<qtrader::itch50::PsxWtsAdapter>(
              trading_date, locator, symbol);
        }
        if (adapter != nullptr && size >= 11 &&
            qtrader::itch50::read_u16(message + 1) == locator) {
          ++target_messages;
          adapter->on_message(message, size, sink);
        }
      });
  const auto stopped = std::chrono::steady_clock::now();
  if (adapter == nullptr) {
    throw std::runtime_error("symbol was not found in Stock Directory messages");
  }
  const auto elapsed = std::chrono::duration<double>(stopped - started).count();
  const auto& stats = adapter->stats();

  std::cout << "Symbol: " << symbol << '\n'
            << "Stock locate: " << locator << '\n'
            << "Trading date: " << trading_date << '\n'
            << "Target ITCH messages: " << target_messages << '\n'
            << "WTS order details: " << stats.order_details << '\n'
            << "WTS transactions: " << stats.transactions << '\n'
            << "  Matches: " << stats.matches << '\n'
            << "  Cancels: " << stats.cancels << '\n'
            << "  Trade busts: " << stats.trade_busts << '\n'
            << "Duplicate matches suppressed: "
            << stats.duplicate_matches_suppressed << '\n'
            << "Non-printable executions suppressed: "
            << stats.non_printable_suppressed << '\n'
            << "Trade busts emitted: " << stats.trade_busts << '\n'
            << "Broken trades without prior output: "
            << stats.broken_trades_without_output << '\n'
            << "Unknown broken trades: " << stats.unknown_broken_trades << '\n'
            << "Peak active orders: " << stats.peak_active_orders << '\n'
            << "End active orders: " << adapter->active_orders() << '\n'
            << "Adapter errors: " << stats.errors << '\n'
            << "Sink errors: " << sink.errors << '\n'
            << "Sink transaction volume: " << sink.volume << '\n'
            << "Sink checksum: " << sink.checksum << '\n'
            << "Elapsed: " << std::fixed << std::setprecision(6) << elapsed
            << " s\n"
            << "Whole-file decode rate: " << std::setprecision(2)
            << static_cast<double>(scan.messages) / elapsed / 1'000'000.0
            << " Mmsg/s\n"
            << "WTS callback rate: "
            << static_cast<double>(stats.order_details + stats.transactions) /
                   elapsed / 1'000'000.0
            << " Mcallback/s\n";

  return stats.errors == 0 && sink.errors == 0 &&
                 adapter->active_orders() == 0 &&
                 stats.order_details == sink.order_details &&
                 stats.transactions == sink.transactions
             ? 0
             : 3;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " HISTORICAL_ITCH50_FILE SYMBOL YYYYMMDD\n";
    return 64;
  }
  const auto date_value = std::strtoull(argv[3], nullptr, 10);
  if (date_value == 0 || date_value > std::numeric_limits<std::uint32_t>::max()) {
    std::cerr << "error: invalid trading date\n";
    return 64;
  }
  try {
    return run(argv[1], argv[2], static_cast<std::uint32_t>(date_value));
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
