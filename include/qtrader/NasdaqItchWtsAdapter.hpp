#pragma once

#include "qtrader/NasdaqItch50.hpp"
#ifdef QTRADER_EXTERNAL_WONDERTRADER
#include "WTSStruct.h"
#else
#include "third_party/wondertrader/WTSStruct.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace qtrader::itch50 {

inline constexpr std::uint32_t kWtsTradeBust = 'B';

static_assert(sizeof(wtp::WTSOrdDtlStruct) == 96,
              "WonderTrader WTSOrdDtlStruct ABI changed");
static_assert(sizeof(wtp::WTSTransStruct) == 112,
              "WonderTrader WTSTransStruct ABI changed");
static_assert(offsetof(wtp::WTSOrdDtlStruct, index) == 64 &&
                  offsetof(wtp::WTSOrdDtlStruct, price) == 72 &&
                  offsetof(wtp::WTSOrdDtlStruct, otype) == 88,
              "WonderTrader WTSOrdDtlStruct field layout changed");
static_assert(offsetof(wtp::WTSTransStruct, index) == 64 &&
                  offsetof(wtp::WTSTransStruct, price) == 80 &&
                  offsetof(wtp::WTSTransStruct, askorder) == 96 &&
                  offsetof(wtp::WTSTransStruct, bidorder) == 104,
              "WonderTrader WTSTransStruct field layout changed");

struct WtsAdapterStats {
  std::uint64_t order_details{0};
  std::uint64_t transactions{0};
  std::uint64_t matches{0};
  std::uint64_t cancels{0};
  std::uint64_t trade_busts{0};
  std::uint64_t duplicate_matches_suppressed{0};
  std::uint64_t non_printable_suppressed{0};
  std::uint64_t broken_trades_without_output{0};
  std::uint64_t unknown_broken_trades{0};
  std::uint64_t errors{0};
  std::uint64_t peak_active_orders{0};
};

class PsxWtsAdapter {
 public:
  PsxWtsAdapter(const std::uint32_t trading_date, const std::uint16_t locator,
                const std::string_view symbol)
      : trading_date_(trading_date), locator_(locator) {
    copy_text(symbol_, symbol);
    orders_.max_load_factor(0.70F);
    orders_.reserve(1024);
    matches_.max_load_factor(0.70F);
    matches_.reserve(4096);
  }

  template <typename Sink>
  void on_message(const std::uint8_t* message, const std::size_t size, Sink& sink) {
    if (size < 11 || read_u16(message + 1) != locator_) {
      return;
    }
    ++event_sequence_;
    const auto expected = expected_message_size(message[0]);
    if (expected == 0 || size != expected) {
      ++stats_.errors;
      return;
    }
    switch (message[0]) {
      case 'A':
      case 'F':
        add(message, sink);
        break;
      case 'E':
      case 'C':
        execute(message, sink);
        break;
      case 'X':
        cancel_partial(message, sink);
        break;
      case 'D':
        cancel_all(message, sink);
        break;
      case 'U':
        replace(message, sink);
        break;
      case 'P':
        non_cross_trade(message, sink);
        break;
      case 'Q':
        cross_trade(message, sink);
        break;
      case 'B':
        broken_trade(message, sink);
        break;
      default:
        break;
    }
  }

  const WtsAdapterStats& stats() const noexcept { return stats_; }
  std::size_t active_orders() const noexcept { return orders_.size(); }

 private:
  struct Order {
    std::uint32_t price{0};
    std::uint32_t remaining{0};
    std::uint32_t side{BDT_Unknown};
  };

  struct Match {
    std::uint32_t price{0};
    std::uint32_t volume{0};
    std::uint64_t order_id{0};
    std::uint32_t side{BDT_Unknown};
    bool emitted{false};
  };

  template <std::size_t Size>
  static void copy_text(std::array<char, Size>& destination,
                        const std::string_view source) noexcept {
    destination.fill('\0');
    const auto count = std::min(source.size(), Size - 1);
    std::memcpy(destination.data(), source.data(), count);
  }

  template <std::size_t Size>
  static void copy_text(char (&destination)[Size],
                        const std::string_view source) noexcept {
    std::memset(destination, 0, Size);
    const auto count = std::min(source.size(), Size - 1);
    std::memcpy(destination, source.data(), count);
  }

  static std::uint32_t action_time(const std::uint64_t timestamp_ns) noexcept {
    const auto milliseconds = timestamp_ns / 1'000'000ULL;
    const auto hours = milliseconds / 3'600'000ULL;
    const auto minutes = (milliseconds / 60'000ULL) % 60ULL;
    const auto seconds = (milliseconds / 1'000ULL) % 60ULL;
    const auto millis = milliseconds % 1'000ULL;
    return static_cast<std::uint32_t>(hours * 10'000'000ULL +
                                      minutes * 100'000ULL +
                                      seconds * 1'000ULL + millis);
  }

  template <typename Struct>
  void fill_common(Struct& output, const std::uint8_t* message) const noexcept {
    copy_text(output.exchg, "PSX");
    copy_text(output.code, std::string_view(symbol_.data()));
    output.trading_date = trading_date_;
    output.action_date = trading_date_;
    output.action_time = action_time(read_u48(message + 5));
  }

  template <typename Sink>
  void emit_order(const std::uint8_t* message, const std::uint64_t order_id,
                  const Order& order, Sink& sink) {
    wtp::WTSOrdDtlStruct output;
    fill_common(output, message);
    output.index = order_id;
    output.price = static_cast<double>(order.price) / 10000.0;
    output.volume = order.remaining;
    output.side = order.side;
    output.otype = ODT_LimitPrice;
    sink.on_order_detail(output);
    ++stats_.order_details;
  }

  template <typename Sink>
  void emit_transaction(const std::uint8_t* message, const std::int64_t index,
                        const std::uint32_t type, const std::uint32_t side,
                        const std::uint32_t price, const std::uint32_t volume,
                        const std::uint64_t order_id, Sink& sink) {
    if (volume == 0 || order_id > static_cast<std::uint64_t>(
                                      std::numeric_limits<std::int64_t>::max())) {
      ++stats_.errors;
      return;
    }
    wtp::WTSTransStruct output;
    fill_common(output, message);
    output.index = index;
    output.ttype = type;
    output.side = side;
    output.price = static_cast<double>(price) / 10000.0;
    output.volume = volume;
    output.bidorder = side == BDT_Buy ? static_cast<std::int64_t>(order_id) : 0;
    output.askorder = side == BDT_Sell ? static_cast<std::int64_t>(order_id) : 0;
    sink.on_transaction(output);
    ++stats_.transactions;
    if (type == TT_Cancel) {
      ++stats_.cancels;
    } else if (type == TT_Match) {
      ++stats_.matches;
    } else if (type == kWtsTradeBust) {
      ++stats_.trade_busts;
    }
  }

  template <typename Sink>
  void add(const std::uint8_t* message, Sink& sink) {
    const auto order_id = read_u64(message + 11);
    const auto shares = read_u32(message + 20);
    const auto price = read_u32(message + 32);
    const auto side = static_cast<std::uint32_t>(message[19]);
    if (order_id == 0 || shares == 0 || price == 0 ||
        (side != BDT_Buy && side != BDT_Sell)) {
      ++stats_.errors;
      return;
    }
    const auto [found, inserted] =
        orders_.emplace(order_id, Order{price, shares, side});
    if (!inserted) {
      ++stats_.errors;
      return;
    }
    stats_.peak_active_orders =
        std::max(stats_.peak_active_orders,
                 static_cast<std::uint64_t>(orders_.size()));
    emit_order(message, order_id, found->second, sink);
  }

  template <typename Sink>
  void execute(const std::uint8_t* message, Sink& sink) {
    const auto order_id = read_u64(message + 11);
    const auto shares = read_u32(message + 19);
    const auto found = orders_.find(order_id);
    if (found == orders_.end() || shares == 0 || shares > found->second.remaining) {
      ++stats_.errors;
      return;
    }
    const auto match_number = read_u64(message + 23);
    const auto execution_price = message[0] == 'C' ? read_u32(message + 32)
                                                    : found->second.price;
    const bool printable = message[0] != 'C' || message[31] == 'Y';
    const Match match{execution_price, shares, order_id,
                      found->second.side, printable};
    const auto [unused, first_match] = matches_.emplace(match_number, match);
    (void)unused;
    if (!printable) {
      ++stats_.non_printable_suppressed;
    } else if (first_match) {
      if (match_number > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
        ++stats_.errors;
      } else {
        emit_transaction(message, static_cast<std::int64_t>(match_number), TT_Match,
                         found->second.side, execution_price, shares, order_id, sink);
      }
    } else {
      ++stats_.duplicate_matches_suppressed;
    }
    found->second.remaining -= shares;
    if (found->second.remaining == 0) {
      orders_.erase(found);
    }
  }

  template <typename Sink>
  void cancel_partial(const std::uint8_t* message, Sink& sink) {
    const auto order_id = read_u64(message + 11);
    const auto shares = read_u32(message + 19);
    const auto found = orders_.find(order_id);
    if (found == orders_.end() || shares == 0 || shares > found->second.remaining) {
      ++stats_.errors;
      return;
    }
    emit_transaction(message, static_cast<std::int64_t>(event_sequence_), TT_Cancel,
                     found->second.side, found->second.price, shares, order_id, sink);
    found->second.remaining -= shares;
    if (found->second.remaining == 0) {
      orders_.erase(found);
    }
  }

  template <typename Sink>
  void cancel_all(const std::uint8_t* message, Sink& sink) {
    const auto order_id = read_u64(message + 11);
    const auto found = orders_.find(order_id);
    if (found == orders_.end()) {
      ++stats_.errors;
      return;
    }
    emit_transaction(message, static_cast<std::int64_t>(event_sequence_), TT_Cancel,
                     found->second.side, found->second.price,
                     found->second.remaining, order_id, sink);
    orders_.erase(found);
  }

  template <typename Sink>
  void replace(const std::uint8_t* message, Sink& sink) {
    const auto old_order_id = read_u64(message + 11);
    const auto new_order_id = read_u64(message + 19);
    const auto shares = read_u32(message + 27);
    const auto price = read_u32(message + 31);
    const auto found = orders_.find(old_order_id);
    if (found == orders_.end() || new_order_id == 0 || shares == 0 || price == 0 ||
        orders_.find(new_order_id) != orders_.end()) {
      ++stats_.errors;
      return;
    }
    const auto old_order = found->second;
    emit_transaction(message, static_cast<std::int64_t>(event_sequence_), TT_Cancel,
                     old_order.side, old_order.price, old_order.remaining,
                     old_order_id, sink);
    orders_.erase(found);
    const auto [replacement, inserted] =
        orders_.emplace(new_order_id, Order{price, shares, old_order.side});
    if (!inserted) {
      ++stats_.errors;
      return;
    }
    emit_order(message, new_order_id, replacement->second, sink);
  }

  template <typename Sink>
  void non_cross_trade(const std::uint8_t* message, Sink& sink) {
    const auto match_number = read_u64(message + 36);
    const Match match{read_u32(message + 32), read_u32(message + 20),
                      read_u64(message + 11),
                      static_cast<std::uint32_t>(message[19]), true};
    const auto [unused, first_match] = matches_.emplace(match_number, match);
    (void)unused;
    if (!first_match) {
      ++stats_.duplicate_matches_suppressed;
      return;
    }
    if (match_number > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
      ++stats_.errors;
      return;
    }
    emit_transaction(message, static_cast<std::int64_t>(match_number), TT_Match,
                     match.side, match.price, match.volume, match.order_id, sink);
  }

  template <typename Sink>
  void cross_trade(const std::uint8_t* message, Sink& sink) {
    const auto shares64 = read_u64(message + 11);
    const auto match_number = read_u64(message + 31);
    if (shares64 == 0 || shares64 > std::numeric_limits<std::uint32_t>::max() ||
        match_number > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
      ++stats_.errors;
      return;
    }
    const Match match{read_u32(message + 27),
                      static_cast<std::uint32_t>(shares64), 0,
                      BDT_Unknown, true};
    const auto [unused, first_match] = matches_.emplace(match_number, match);
    (void)unused;
    if (!first_match) {
      ++stats_.duplicate_matches_suppressed;
      return;
    }
    emit_transaction(message, static_cast<std::int64_t>(match_number), TT_Match,
                     match.side, match.price, match.volume, match.order_id, sink);
  }

  template <typename Sink>
  void broken_trade(const std::uint8_t* message, Sink& sink) {
    const auto match_number = read_u64(message + 11);
    const auto found = matches_.find(match_number);
    if (found == matches_.end()) {
      ++stats_.unknown_broken_trades;
      return;
    }
    const auto match = found->second;
    if (match.emitted) {
      emit_transaction(message, static_cast<std::int64_t>(match_number),
                       kWtsTradeBust, match.side, match.price, match.volume,
                       match.order_id, sink);
    } else {
      ++stats_.broken_trades_without_output;
    }
    matches_.erase(found);
  }

  std::uint32_t trading_date_{0};
  std::uint16_t locator_{0};
  std::array<char, 9> symbol_{};
  std::uint64_t event_sequence_{0};
  std::unordered_map<std::uint64_t, Order> orders_;
  std::unordered_map<std::uint64_t, Match> matches_;
  WtsAdapterStats stats_{};
};

}  // namespace qtrader::itch50
