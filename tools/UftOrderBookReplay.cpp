#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/UftOrderBookPipeline.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
std::uint64_t number(const char* text) {
  std::string_view s(text); std::uint64_t value = 0;
  auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  if (result.ec != std::errc{} || result.ptr != s.data() + s.size() || !value)
    throw std::invalid_argument("arguments must be positive decimal integers");
  return value;
}
struct Strategy {
  std::uint64_t callbacks{0};
  qtrader::PriceTicks bid{0}, ask{0};
  qtrader::Quantity bid_quantity{0}, ask_quantity{0};
  static void on_book(void* self, const qtrader::UftOrderBookPipeline::Book& book,
      const qtrader::UftBookUpdate*, std::uint64_t, qtrader::Quantity) noexcept {
    auto& strategy = *static_cast<Strategy*>(self);
    ++strategy.callbacks;
    strategy.bid = book.best_bid(); strategy.ask = book.best_ask();
    strategy.bid_quantity = book.level_quantity(wthft::Side::Buy, strategy.bid);
    strategy.ask_quantity = book.level_quantity(wthft::Side::Sell, strategy.ask);
  }
};
}
int main(int argc, char** argv) {
  if (argc != 8) {
    std::cerr << "Usage: " << argv[0]
      << " full-session.itch locator min_price max_price tick_size max_orders max_matches\n"
      << "Prices use ITCH 1/10000 dollars; input MUST start at a trusted empty-book boundary.\n";
    return 1;
  }
  try {
    const auto locator = number(argv[2]), minimum = number(argv[3]), maximum = number(argv[4]);
    const auto tick = number(argv[5]), orders = number(argv[6]), matches = number(argv[7]);
    if (locator > UINT16_MAX || maximum > INT64_MAX || minimum > INT64_MAX || tick > INT64_MAX)
      throw std::invalid_argument("argument out of range");
    qtrader::UftOrderBookPipeline pipeline("FILESESSION", 1, locator, orders, matches,
                                         minimum, maximum, tick);
    Strategy strategy;
    if (!pipeline.start(&strategy, Strategy::on_book)) throw std::runtime_error("start failed");
    const auto scanned = qtrader::itch50::scan_historical_file(argv[1], [&](auto data, auto size) {
      if (pipeline.ingest(pipeline.session(), pipeline.next_sequence(), data, size) !=
          qtrader::BookIngestResult::Applied)
        throw std::runtime_error(pipeline.error() ? pipeline.error() : "feed not ready");
    });
    if (!strategy.callbacks || strategy.callbacks != pipeline.strategy_callbacks())
      throw std::runtime_error("no verified book strategy delivery; check locator/input");
    if (strategy.bid != pipeline.book().best_bid() || strategy.ask != pipeline.book().best_ask() ||
        strategy.bid_quantity != pipeline.book().level_quantity(wthft::Side::Buy, strategy.bid) ||
        strategy.ask_quantity != pipeline.book().level_quantity(wthft::Side::Sell, strategy.ask))
      throw std::runtime_error("strategy/book final view mismatch");
    std::cout << "replay=PASS messages=" << scanned.messages << " book_callbacks=" << strategy.callbacks
      << " active_orders=" << pipeline.book().active_orders() << " best_bid=" << pipeline.book().best_bid()
      << " best_ask=" << pipeline.book().best_ask() << " net_printed_volume=" << pipeline.net_printed_volume()
      << "\nFile ordinals cannot detect messages missing from the source file; no performance claim.\n";
  } catch (const std::exception& e) { std::cerr << "replay=FAIL " << e.what() << '\n'; return 2; }
}
