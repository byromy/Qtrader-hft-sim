#pragma once

#include <cstddef>
#include <cstdint>

namespace qtrader::itch50 {

inline std::uint16_t read_u16(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint16_t>(p[0]) << 8U) |
         static_cast<std::uint16_t>(p[1]);
}

inline std::uint32_t read_u32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24U) |
         (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) |
         static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t read_u48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(p[0]) << 40U) |
         (static_cast<std::uint64_t>(p[1]) << 32U) |
         (static_cast<std::uint64_t>(p[2]) << 24U) |
         (static_cast<std::uint64_t>(p[3]) << 16U) |
         (static_cast<std::uint64_t>(p[4]) << 8U) |
         static_cast<std::uint64_t>(p[5]);
}

inline std::uint64_t read_u64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(p[0]) << 56U) |
         (static_cast<std::uint64_t>(p[1]) << 48U) |
         (static_cast<std::uint64_t>(p[2]) << 40U) |
         (static_cast<std::uint64_t>(p[3]) << 32U) |
         (static_cast<std::uint64_t>(p[4]) << 24U) |
         (static_cast<std::uint64_t>(p[5]) << 16U) |
         (static_cast<std::uint64_t>(p[6]) << 8U) |
         static_cast<std::uint64_t>(p[7]);
}

// Message sizes include the one-byte ITCH message type, but not the historical
// file's two-byte big-endian record-length prefix.
constexpr std::size_t expected_message_size(const std::uint8_t type) noexcept {
  switch (type) {
    case 'S': return 12;  // System Event
    case 'R': return 39;  // Stock Directory
    case 'H': return 25;  // Stock Trading Action
    case 'Y': return 20;  // Reg SHO Restriction
    case 'L': return 26;  // Market Participant Position
    case 'V': return 35;  // MWCB Decline Level
    case 'W': return 12;  // MWCB Status
    case 'K': return 28;  // IPO Quoting Period Update
    case 'J': return 35;  // LULD Auction Collar
    case 'h': return 21;  // Operational Halt
    case 'A': return 36;  // Add Order
    case 'F': return 40;  // Add Order with MPID Attribution
    case 'E': return 31;  // Order Executed
    case 'C': return 36;  // Order Executed with Price
    case 'X': return 23;  // Order Cancel
    case 'D': return 19;  // Order Delete
    case 'U': return 35;  // Order Replace
    case 'P': return 44;  // Trade
    case 'Q': return 40;  // Cross Trade
    case 'B': return 19;  // Broken Trade
    case 'I': return 50;  // Net Order Imbalance Indicator
    case 'N': return 20;  // Retail Price Improvement Indicator
    default: return 0;
  }
}

constexpr const char* message_name(const std::uint8_t type) noexcept {
  switch (type) {
    case 'S': return "System Event";
    case 'R': return "Stock Directory";
    case 'H': return "Stock Trading Action";
    case 'Y': return "Reg SHO Restriction";
    case 'L': return "Market Participant Position";
    case 'V': return "MWCB Decline Level";
    case 'W': return "MWCB Status";
    case 'K': return "IPO Quoting Period Update";
    case 'J': return "LULD Auction Collar";
    case 'h': return "Operational Halt";
    case 'A': return "Add Order";
    case 'F': return "Add Order with MPID";
    case 'E': return "Order Executed";
    case 'C': return "Order Executed with Price";
    case 'X': return "Order Cancel";
    case 'D': return "Order Delete";
    case 'U': return "Order Replace";
    case 'P': return "Trade";
    case 'Q': return "Cross Trade";
    case 'B': return "Broken Trade";
    case 'I': return "NOII";
    case 'N': return "Retail Price Improvement";
    default: return "Unknown";
  }
}

}  // namespace qtrader::itch50
