#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/NasdaqItchOrderLifecycle.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

struct Stats {
  std::array<std::uint64_t, 256> type_counts{};
  std::array<std::uint64_t, 65536> locator_counts{};
  std::array<std::string, 65536> symbols{};
  std::uint64_t messages{0};
  std::uint64_t framed_bytes{0};
  std::uint64_t payload_bytes{0};
  std::uint64_t unknown_types{0};
  std::uint64_t size_mismatches{0};
  std::uint64_t timestamp_regressions{0};
  std::uint64_t first_timestamp{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t last_timestamp{0};
  std::uint64_t previous_timestamp{0};
  bool have_timestamp{false};
};

std::string trim_symbol(const std::uint8_t* bytes, const std::size_t length) {
  std::size_t end = length;
  while (end != 0 && bytes[end - 1] == ' ') {
    --end;
  }
  return {reinterpret_cast<const char*>(bytes), end};
}

std::string format_timestamp(const std::uint64_t ns) {
  constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
  const auto seconds = ns / kNsPerSecond;
  const auto nanos = ns % kNsPerSecond;
  const auto hours = seconds / 3600U;
  const auto minutes = (seconds / 60U) % 60U;
  const auto secs = seconds % 60U;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
      << minutes << ':' << std::setw(2) << secs << '.' << std::setw(9) << nanos;
  return out.str();
}

void account_message(const std::uint8_t* message, const std::size_t size,
                     Stats& stats, qtrader::itch50::OrderLifecycle* lifecycle) {
  const auto type = message[0];
  ++stats.messages;
  ++stats.type_counts[type];
  stats.payload_bytes += size;

  const auto expected = qtrader::itch50::expected_message_size(type);
  if (expected == 0) {
    ++stats.unknown_types;
  } else if (expected != size) {
    ++stats.size_mismatches;
  }

  if (size >= 11) {
    const auto locator = qtrader::itch50::read_u16(message + 1);
    ++stats.locator_counts[locator];
    const auto timestamp = qtrader::itch50::read_u48(message + 5);
    if (stats.have_timestamp && timestamp < stats.previous_timestamp) {
      ++stats.timestamp_regressions;
    }
    if (!stats.have_timestamp) {
      stats.first_timestamp = timestamp;
      stats.have_timestamp = true;
    }
    stats.previous_timestamp = timestamp;
    stats.last_timestamp = timestamp;
  }

  if (type == 'R' && size == 39) {
    const auto locator = qtrader::itch50::read_u16(message + 1);
    stats.symbols[locator] = trim_symbol(message + 11, 8);
  }
  if (lifecycle != nullptr) {
    lifecycle->on_message(message, size);
  }
}

void print_stats(const char* path, const std::uint64_t file_size, const Stats& stats,
                 const qtrader::itch50::OrderLifecycle* lifecycle,
                 const double elapsed_seconds) {
  const double mib = static_cast<double>(file_size) / (1024.0 * 1024.0);
  std::cout << "File: " << path << '\n'
            << "File bytes: " << file_size << " (" << std::fixed
            << std::setprecision(2) << mib << " MiB)\n"
            << "Messages: " << stats.messages << '\n'
            << "Payload bytes: " << stats.payload_bytes << '\n'
            << "Framing bytes: " << (stats.framed_bytes - stats.payload_bytes)
            << '\n'
            << "Unknown message types: " << stats.unknown_types << '\n'
            << "Message-size mismatches: " << stats.size_mismatches << '\n'
            << "Timestamp regressions: " << stats.timestamp_regressions << '\n';
  if (stats.have_timestamp) {
    std::cout << "First timestamp: " << format_timestamp(stats.first_timestamp)
              << '\n'
              << "Last timestamp: " << format_timestamp(stats.last_timestamp)
              << '\n';
  }
  std::cout << "Elapsed: " << std::setprecision(6) << elapsed_seconds << " s\n"
            << "Scan throughput: " << std::setprecision(2)
            << (mib / elapsed_seconds) << " MiB/s\n"
            << "Decode rate: " << std::setprecision(2)
            << (static_cast<double>(stats.messages) / elapsed_seconds / 1'000'000.0)
            << " Mmsg/s\n\n"
            << "Type  Size  Count        Description\n";

  for (std::size_t i = 0; i < stats.type_counts.size(); ++i) {
    if (stats.type_counts[i] == 0) {
      continue;
    }
    const auto type = static_cast<std::uint8_t>(i);
    std::cout << std::left << std::setw(6) << static_cast<char>(type)
              << std::setw(6) << qtrader::itch50::expected_message_size(type)
              << std::setw(13) << stats.type_counts[i]
              << qtrader::itch50::message_name(type) << '\n';
  }

  std::vector<std::pair<std::uint64_t, std::uint16_t>> active;
  active.reserve(stats.symbols.size());
  for (std::size_t locator = 0; locator < stats.symbols.size(); ++locator) {
    if (!stats.symbols[locator].empty() && stats.locator_counts[locator] != 0) {
      active.emplace_back(stats.locator_counts[locator],
                          static_cast<std::uint16_t>(locator));
    }
  }
  const auto top_count = std::min<std::size_t>(10, active.size());
  const auto top_end = active.begin() +
                       static_cast<std::vector<std::pair<std::uint64_t,
                                                         std::uint16_t>>::difference_type>(
                           top_count);
  std::partial_sort(active.begin(), top_end, active.end(),
                    std::greater<>());
  std::cout << "\nSymbols discovered: " << active.size() << '\n'
            << "Top symbols by all ITCH messages:\n";
  for (std::size_t i = 0; i < top_count; ++i) {
    const auto [count, locator] = active[i];
    std::cout << "  " << std::left << std::setw(8) << stats.symbols[locator]
              << " locator=" << std::setw(5) << locator << " messages=" << count
              << '\n';
  }
  if (lifecycle != nullptr) {
    const auto& life = lifecycle->stats();
    std::cout << "\nOrder lifecycle validation:\n"
              << "  Adds: " << life.adds << '\n'
              << "  Executions: " << life.executions
              << " (shares=" << life.executed_shares << ")\n"
              << "  Partial cancels: " << life.cancels
              << " (shares=" << life.canceled_shares << ")\n"
              << "  Deletes: " << life.deletes << '\n'
              << "  Replaces: " << life.replaces << '\n'
              << "  Peak active orders: " << life.peak_active_orders << '\n'
              << "  End active orders: " << lifecycle->active_orders() << '\n'
              << "  Duplicate order IDs: " << life.duplicate_order_ids << '\n'
              << "  Missing order references: " << life.missing_order_ids << '\n'
              << "  Quantity over-reductions: " << life.over_reductions << '\n'
              << "  Stock-locate mismatches: " << life.stock_locate_mismatches << '\n'
              << "  Invalid adds/replacements: " << life.invalid_adds << '\n';
  }
}

int inspect(const char* path, const bool validate_lifecycle) {
  struct stat file_info {};
  if (::stat(path, &file_info) != 0) {
    throw std::runtime_error(std::string("stat failed: ") + std::strerror(errno));
  }
  if (!S_ISREG(file_info.st_mode)) {
    throw std::runtime_error("input is not a regular file");
  }

  Stats stats;
  std::unique_ptr<qtrader::itch50::OrderLifecycle> lifecycle;
  if (validate_lifecycle) {
    lifecycle = std::make_unique<qtrader::itch50::OrderLifecycle>();
  }
  const auto started = std::chrono::steady_clock::now();
  const auto scan = qtrader::itch50::scan_historical_file(
      path, [&](const std::uint8_t* message, const std::size_t size) {
        account_message(message, size, stats, lifecycle.get());
      });
  stats.framed_bytes = scan.framed_bytes;
  const auto stopped = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double>(stopped - started).count();
  print_stats(path, static_cast<std::uint64_t>(file_info.st_size), stats,
              lifecycle.get(), elapsed);

  if (stats.framed_bytes != static_cast<std::uint64_t>(file_info.st_size)) {
    std::cerr << "error: parsed byte count does not equal file size\n";
    return 2;
  }
  const bool lifecycle_valid =
      lifecycle == nullptr ||
      (lifecycle->stats().duplicate_order_ids == 0 &&
       lifecycle->stats().missing_order_ids == 0 &&
       lifecycle->stats().over_reductions == 0 &&
       lifecycle->stats().stock_locate_mismatches == 0 &&
       lifecycle->stats().invalid_adds == 0);
  return (stats.unknown_types == 0 && stats.size_mismatches == 0 &&
          lifecycle_valid)
             ? 0
             : 3;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 ||
      (argc == 3 && std::string_view(argv[2]) != "--lifecycle")) {
    std::cerr << "Usage: " << argv[0]
              << " HISTORICAL_ITCH50_FILE [--lifecycle]\n";
    return 64;
  }
  try {
    return inspect(argv[1], argc == 3);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
