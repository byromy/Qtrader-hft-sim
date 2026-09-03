#pragma once

#include "qtrader/NasdaqItch50.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace qtrader::itch50 {

struct OrderRecord {
  std::uint32_t price{0};       // ITCH Price(4), i.e. integer units of 0.0001 USD.
  std::uint32_t remaining{0};
  std::uint16_t stock_locate{0};
  std::uint8_t side{0};         // ASCII 'B' or 'S'.
};

struct LifecycleStats {
  std::uint64_t adds{0};
  std::uint64_t executions{0};
  std::uint64_t executed_shares{0};
  std::uint64_t cancels{0};
  std::uint64_t canceled_shares{0};
  std::uint64_t deletes{0};
  std::uint64_t replaces{0};
  std::uint64_t duplicate_order_ids{0};
  std::uint64_t missing_order_ids{0};
  std::uint64_t over_reductions{0};
  std::uint64_t stock_locate_mismatches{0};
  std::uint64_t invalid_adds{0};
  std::uint64_t peak_active_orders{0};
};

class OrderLifecycle {
 public:
  explicit OrderLifecycle(const std::size_t expected_peak_orders = 1'000'000) {
    orders_.max_load_factor(0.70F);
    orders_.reserve(expected_peak_orders);
  }

  void on_message(const std::uint8_t* message, const std::size_t size) {
    if (size == 0) {
      return;
    }
    switch (message[0]) {
      case 'A':
      case 'F':
        if (size == expected_message_size(message[0])) {
          add(message);
        }
        break;
      case 'E':
      case 'C':
        if (size == expected_message_size(message[0])) {
          reduce(message, read_u32(message + 19), Reduction::Execution);
        }
        break;
      case 'X':
        if (size == expected_message_size('X')) {
          reduce(message, read_u32(message + 19), Reduction::Cancel);
        }
        break;
      case 'D':
        if (size == expected_message_size('D')) {
          erase(message);
        }
        break;
      case 'U':
        if (size == expected_message_size('U')) {
          replace(message);
        }
        break;
      default:
        break;
    }
  }

  const LifecycleStats& stats() const noexcept { return stats_; }
  std::size_t active_orders() const noexcept { return orders_.size(); }

 private:
  enum class Reduction { Execution, Cancel };

  void update_peak() noexcept {
    stats_.peak_active_orders =
        std::max(stats_.peak_active_orders,
                 static_cast<std::uint64_t>(orders_.size()));
  }

  void add(const std::uint8_t* message) {
    ++stats_.adds;
    const auto order_id = read_u64(message + 11);
    const auto side = message[19];
    const auto shares = read_u32(message + 20);
    const auto price = read_u32(message + 32);
    const auto stock_locate = read_u16(message + 1);
    if (order_id == 0 || shares == 0 || price == 0 ||
        (side != 'B' && side != 'S')) {
      ++stats_.invalid_adds;
      return;
    }
    const auto [unused, inserted] = orders_.emplace(
        order_id, OrderRecord{price, shares, stock_locate, side});
    (void)unused;
    if (!inserted) {
      ++stats_.duplicate_order_ids;
      return;
    }
    update_peak();
  }

  void reduce(const std::uint8_t* message, const std::uint32_t shares,
              const Reduction kind) {
    if (kind == Reduction::Execution) {
      ++stats_.executions;
      stats_.executed_shares += shares;
    } else {
      ++stats_.cancels;
      stats_.canceled_shares += shares;
    }
    const auto order_id = read_u64(message + 11);
    const auto found = orders_.find(order_id);
    if (found == orders_.end()) {
      ++stats_.missing_order_ids;
      return;
    }
    if (found->second.stock_locate != read_u16(message + 1)) {
      ++stats_.stock_locate_mismatches;
    }
    if (shares == 0 || shares > found->second.remaining) {
      ++stats_.over_reductions;
      return;
    }
    found->second.remaining -= shares;
    if (found->second.remaining == 0) {
      orders_.erase(found);
    }
  }

  void erase(const std::uint8_t* message) {
    ++stats_.deletes;
    const auto order_id = read_u64(message + 11);
    const auto found = orders_.find(order_id);
    if (found == orders_.end()) {
      ++stats_.missing_order_ids;
      return;
    }
    if (found->second.stock_locate != read_u16(message + 1)) {
      ++stats_.stock_locate_mismatches;
    }
    orders_.erase(found);
  }

  void replace(const std::uint8_t* message) {
    ++stats_.replaces;
    const auto old_order_id = read_u64(message + 11);
    const auto new_order_id = read_u64(message + 19);
    const auto shares = read_u32(message + 27);
    const auto price = read_u32(message + 31);
    const auto found = orders_.find(old_order_id);
    if (found == orders_.end()) {
      ++stats_.missing_order_ids;
      return;
    }
    if (found->second.stock_locate != read_u16(message + 1)) {
      ++stats_.stock_locate_mismatches;
    }
    if (new_order_id == 0 || shares == 0 || price == 0) {
      ++stats_.invalid_adds;
      return;
    }
    if (orders_.find(new_order_id) != orders_.end()) {
      ++stats_.duplicate_order_ids;
      return;
    }
    auto replacement = found->second;
    replacement.price = price;
    replacement.remaining = shares;
    orders_.erase(found);
    orders_.emplace(new_order_id, replacement);
    update_peak();
  }

  std::unordered_map<std::uint64_t, OrderRecord> orders_;
  LifecycleStats stats_{};
};

}  // namespace qtrader::itch50
