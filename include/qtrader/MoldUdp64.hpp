#pragma once

#include "qtrader/NasdaqItch50.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qtrader::moldudp64 {

constexpr std::size_t kSessionBytes = 10;
constexpr std::size_t kHeaderBytes = 20;
constexpr std::uint16_t kEndOfSession = 0xffffU;

using Session = std::array<std::uint8_t, kSessionBytes>;

enum class PacketKind : std::uint8_t { data, heartbeat, end_of_session };

struct DownstreamHeader {
  Session session{};
  std::uint64_t first_sequence{0};
  std::uint16_t message_count{0};

  PacketKind kind() const noexcept {
    if (message_count == 0) return PacketKind::heartbeat;
    if (message_count == kEndOfSession) return PacketKind::end_of_session;
    return PacketKind::data;
  }
};

enum class ParseError : std::uint8_t {
  none,
  header_truncated,
  message_length_truncated,
  message_truncated,
  special_packet_has_payload,
  trailing_bytes,
  sequence_overflow,
};

struct ParseResult {
  DownstreamHeader header{};
  ParseError error{ParseError::none};
  std::size_t bytes_consumed{0};
  std::uint16_t messages_visited{0};

  explicit operator bool() const noexcept { return error == ParseError::none; }
};

inline bool decode_header(const std::uint8_t* packet, const std::size_t size,
                          DownstreamHeader& header) noexcept {
  if (packet == nullptr || size < kHeaderBytes) return false;
  std::memcpy(header.session.data(), packet, kSessionBytes);
  header.first_sequence = itch50::read_u64(packet + 10);
  header.message_count = itch50::read_u16(packet + 18);
  return true;
}

// Validates the complete datagram before invoking callback, so malformed packets
// cannot partially mutate an order book. Callback receives (sequence, data, size).
template <typename Callback>
ParseResult parse_downstream_packet(const std::uint8_t* packet,
                                    const std::size_t size,
                                    Callback&& callback) noexcept {
  ParseResult result;
  if (!decode_header(packet, size, result.header)) {
    result.error = ParseError::header_truncated;
    return result;
  }
  result.bytes_consumed = kHeaderBytes;

  const auto count = result.header.message_count;
  if (count == 0 || count == kEndOfSession) {
    if (size != kHeaderBytes) {
      result.error = ParseError::special_packet_has_payload;
    }
    return result;
  }
  if (result.header.first_sequence >
      std::numeric_limits<std::uint64_t>::max() - (count - 1U)) {
    result.error = ParseError::sequence_overflow;
    return result;
  }

  // Pass one: validate every message block and exact packet consumption.
  std::size_t offset = kHeaderBytes;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (size - offset < 2) {
      result.error = ParseError::message_length_truncated;
      result.bytes_consumed = offset;
      return result;
    }
    const auto message_size = itch50::read_u16(packet + offset);
    offset += 2;
    if (size - offset < message_size) {
      result.error = ParseError::message_truncated;
      result.bytes_consumed = offset;
      return result;
    }
    offset += message_size;
  }
  if (offset != size) {
    result.error = ParseError::trailing_bytes;
    result.bytes_consumed = offset;
    return result;
  }

  // Pass two: expose stable views directly into the receive buffer.
  offset = kHeaderBytes;
  for (std::uint16_t i = 0; i < count; ++i) {
    const auto message_size = itch50::read_u16(packet + offset);
    offset += 2;
    callback(result.header.first_sequence + i, packet + offset, message_size);
    offset += message_size;
    ++result.messages_visited;
  }
  result.bytes_consumed = offset;
  return result;
}

enum class SequenceState : std::uint8_t {
  in_order,
  overlap,
  duplicate,
  gap,
  session_mismatch,
  sequence_overflow,
};

struct SequenceDecision {
  SequenceState state{SequenceState::in_order};
  std::uint16_t skip_messages{0};
  std::uint64_t expected_sequence{0};
  std::uint64_t gap_messages{0};

  bool may_process() const noexcept {
    return state == SequenceState::in_order || state == SequenceState::overlap;
  }
};

class SequenceTracker {
 public:
  SequenceDecision inspect(const DownstreamHeader& header) noexcept {
    if (!initialized_) {
      session_ = header.session;
      next_sequence_ = header.first_sequence;
      initialized_ = true;
    } else if (session_ != header.session) {
      return {SequenceState::session_mismatch, 0, next_sequence_, 0};
    }

    const bool carries_data = header.kind() == PacketKind::data;
    const std::uint64_t count = carries_data ? header.message_count : 0;
    if (count != 0 && header.first_sequence >
                          std::numeric_limits<std::uint64_t>::max() - count) {
      return {SequenceState::sequence_overflow, 0, next_sequence_, 0};
    }
    const auto packet_end = header.first_sequence + count;

    if (header.first_sequence > next_sequence_) {
      return {SequenceState::gap, 0, next_sequence_,
              header.first_sequence - next_sequence_};
    }
    if (!carries_data) {
      if (header.first_sequence == next_sequence_) {
        return {SequenceState::in_order, 0, next_sequence_, 0};
      }
      return {SequenceState::duplicate, 0, next_sequence_, 0};
    }
    if (packet_end <= next_sequence_) {
      return {SequenceState::duplicate, header.message_count, next_sequence_, 0};
    }
    if (header.first_sequence < next_sequence_) {
      return {SequenceState::overlap,
              static_cast<std::uint16_t>(next_sequence_ - header.first_sequence),
              next_sequence_, 0};
    }
    return {SequenceState::in_order, 0, next_sequence_, 0};
  }

  void commit(const DownstreamHeader& header,
              const SequenceDecision& decision) noexcept {
    if (!decision.may_process() || header.kind() != PacketKind::data) return;
    next_sequence_ = header.first_sequence + header.message_count;
  }

  bool initialized() const noexcept { return initialized_; }
  const Session& session() const noexcept { return session_; }
  std::uint64_t next_sequence() const noexcept { return next_sequence_; }

 private:
  Session session_{};
  std::uint64_t next_sequence_{0};
  bool initialized_{false};
};

inline void encode_u16(std::uint8_t* output, const std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 8U);
  output[1] = static_cast<std::uint8_t>(value);
}

inline void encode_u64(std::uint8_t* output, const std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    output[i] = static_cast<std::uint8_t>(value >> (56U - i * 8U));
  }
}

inline std::array<std::uint8_t, kHeaderBytes> make_request_packet(
    const Session& session, const std::uint64_t first_sequence,
    const std::uint16_t requested_message_count) noexcept {
  std::array<std::uint8_t, kHeaderBytes> packet{};
  std::memcpy(packet.data(), session.data(), session.size());
  encode_u64(packet.data() + 10, first_sequence);
  encode_u16(packet.data() + 18, requested_message_count);
  return packet;
}

}  // namespace qtrader::moldudp64
