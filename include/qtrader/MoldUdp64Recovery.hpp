#pragma once

#include "qtrader/MoldUdp64.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qtrader::moldudp64 {

enum class RecoveryStatus : std::uint8_t {
  processed,
  buffered_gap,
  duplicate,
  heartbeat,
  end_of_session,
  malformed,
  session_mismatch,
  datagram_too_large,
  reorder_window_full,
  sequence_overflow,
};

struct RecoveryStats {
  std::uint64_t datagrams{0};
  std::uint64_t messages{0};
  std::uint64_t buffered_datagrams{0};
  std::uint64_t recovered_datagrams{0};
  std::uint64_t duplicates{0};
  std::uint64_t gap_events{0};
  std::uint64_t rerequests{0};
  std::uint64_t requested_messages{0};
  std::uint64_t malformed{0};
  std::uint64_t session_mismatches{0};
  std::uint64_t capacity_errors{0};
};

// Fixed-capacity recovery layer for one MoldUDP64 channel. The normal in-order
// path does not allocate or copy a datagram. Only packets observed beyond a gap
// are copied into the bounded reorder window.
template <std::size_t ReorderSlots = 1024,
          std::size_t MaxDatagramBytes = 2048>
class RecoveryWindow {
  static_assert(ReorderSlots > 0, "Recovery window must have at least one slot");
  static_assert(MaxDatagramBytes >= kHeaderBytes,
                "Datagram capacity must fit a MoldUDP64 header");

 public:
  template <typename MessageCallback, typename RequestCallback,
            typename ControlCallback>
  RecoveryStatus ingest(const std::uint8_t* packet, const std::size_t size,
                        MessageCallback&& on_message,
                        RequestCallback&& on_request,
                        ControlCallback&& on_control) {
    ++stats_.datagrams;
    const auto validation = parse_downstream_packet(
        packet, size,
        [](std::uint64_t, const std::uint8_t*, std::size_t) noexcept {});
    if (!validation) {
      ++stats_.malformed;
      return RecoveryStatus::malformed;
    }

    const auto decision = tracker_.inspect(validation.header);
    if (decision.state == SequenceState::session_mismatch) {
      ++stats_.session_mismatches;
      return RecoveryStatus::session_mismatch;
    }
    if (decision.state == SequenceState::sequence_overflow) {
      ++stats_.malformed;
      return RecoveryStatus::sequence_overflow;
    }
    if (decision.state == SequenceState::duplicate) {
      ++stats_.duplicates;
      return RecoveryStatus::duplicate;
    }

    if (decision.state == SequenceState::gap) {
      ++stats_.gap_events;
      gap_high_water_ = std::max(gap_high_water_,
                                 validation.header.first_sequence);
      if (validation.header.kind() == PacketKind::data) {
        const auto stored = store(packet, size, validation.header);
        if (stored != RecoveryStatus::buffered_gap) return stored;
      } else if (validation.header.kind() == PacketKind::end_of_session) {
        pending_end_sequence_ = validation.header.first_sequence;
        has_pending_end_ = true;
      }
      issue_request(on_request);
      return RecoveryStatus::buffered_gap;
    }

    const auto kind = validation.header.kind();
    if (kind == PacketKind::heartbeat) {
      on_control(kind, validation.header.first_sequence);
      return RecoveryStatus::heartbeat;
    }
    if (kind == PacketKind::end_of_session) {
      on_control(kind, validation.header.first_sequence);
      return RecoveryStatus::end_of_session;
    }

    process(packet, size, validation.header, decision, on_message);
    request_outstanding_ = false;
    drain(on_message, on_request, on_control);
    return RecoveryStatus::processed;
  }

  const RecoveryStats& stats() const noexcept { return stats_; }
  std::uint64_t next_sequence() const noexcept {
    return tracker_.next_sequence();
  }
  std::size_t buffered_datagrams() const noexcept { return occupied_; }

 private:
  struct Slot {
    std::array<std::uint8_t, MaxDatagramBytes> bytes{};
    std::uint64_t first_sequence{0};
    std::uint16_t message_count{0};
    std::uint16_t size{0};
    bool occupied{false};
  };

  RecoveryStatus store(const std::uint8_t* packet, const std::size_t size,
                       const DownstreamHeader& header) {
    if (size > MaxDatagramBytes || size >
                                      std::numeric_limits<std::uint16_t>::max()) {
      ++stats_.capacity_errors;
      return RecoveryStatus::datagram_too_large;
    }
    for (const auto& slot : slots_) {
      if (slot.occupied && slot.first_sequence == header.first_sequence) {
        ++stats_.duplicates;
        return RecoveryStatus::duplicate;
      }
    }
    for (auto& slot : slots_) {
      if (slot.occupied) continue;
      std::memcpy(slot.bytes.data(), packet, size);
      slot.first_sequence = header.first_sequence;
      slot.message_count = header.message_count;
      slot.size = static_cast<std::uint16_t>(size);
      slot.occupied = true;
      ++occupied_;
      ++stats_.buffered_datagrams;
      return RecoveryStatus::buffered_gap;
    }
    ++stats_.capacity_errors;
    return RecoveryStatus::reorder_window_full;
  }

  template <typename MessageCallback>
  void process(const std::uint8_t* packet, const std::size_t size,
               const DownstreamHeader& header,
               const SequenceDecision& decision,
               MessageCallback& on_message) {
    std::uint16_t index = 0;
    const auto parsed = parse_downstream_packet(
        packet, size,
        [&](const std::uint64_t sequence, const std::uint8_t* message,
            const std::size_t message_size) {
          if (index++ < decision.skip_messages) return;
          on_message(sequence, message, message_size);
          ++stats_.messages;
        });
    // ingest/store validate every packet before process, so this is invariant.
    if (!parsed) {
      ++stats_.malformed;
      return;
    }
    tracker_.commit(header, decision);
  }

  Slot* next_buffered_slot() noexcept {
    Slot* candidate = nullptr;
    const auto expected = tracker_.next_sequence();
    for (auto& slot : slots_) {
      if (!slot.occupied) continue;
      const auto end = slot.first_sequence + slot.message_count;
      if (slot.first_sequence <= expected && expected < end &&
          (candidate == nullptr ||
           slot.first_sequence > candidate->first_sequence)) {
        candidate = &slot;
      }
    }
    return candidate;
  }

  std::uint64_t earliest_future_sequence() const noexcept {
    auto earliest = gap_high_water_;
    for (const auto& slot : slots_) {
      if (!slot.occupied || slot.first_sequence <= tracker_.next_sequence())
        continue;
      earliest = earliest == 0 ? slot.first_sequence
                               : std::min(earliest, slot.first_sequence);
    }
    if (has_pending_end_) {
      earliest = earliest == 0 ? pending_end_sequence_
                               : std::min(earliest, pending_end_sequence_);
    }
    return earliest;
  }

  template <typename MessageCallback, typename RequestCallback,
            typename ControlCallback>
  void drain(MessageCallback& on_message, RequestCallback& on_request,
             ControlCallback& on_control) {
    while (auto* slot = next_buffered_slot()) {
      DownstreamHeader header;
      if (!decode_header(slot->bytes.data(), slot->size, header)) {
        ++stats_.malformed;
        slot->occupied = false;
        --occupied_;
        continue;
      }
      const auto decision = tracker_.inspect(header);
      if (!decision.may_process()) break;
      process(slot->bytes.data(), slot->size, header, decision, on_message);
      slot->occupied = false;
      --occupied_;
      ++stats_.recovered_datagrams;
      request_outstanding_ = false;
    }

    if (has_pending_end_ && pending_end_sequence_ == tracker_.next_sequence()) {
      on_control(PacketKind::end_of_session, pending_end_sequence_);
      has_pending_end_ = false;
    }
    if (tracker_.next_sequence() >= gap_high_water_) gap_high_water_ = 0;
    issue_request(on_request);
  }

  template <typename RequestCallback>
  void issue_request(RequestCallback& on_request) {
    const auto expected = tracker_.next_sequence();
    const auto target = earliest_future_sequence();
    if (request_outstanding_ || target <= expected) return;
    const auto distance = target - expected;
    const auto count = static_cast<std::uint16_t>(
        std::min<std::uint64_t>(distance, kEndOfSession - 1U));
    on_request(tracker_.session(), expected, count);
    request_outstanding_ = true;
    ++stats_.rerequests;
    stats_.requested_messages += count;
  }

  std::array<Slot, ReorderSlots> slots_{};
  SequenceTracker tracker_{};
  RecoveryStats stats_{};
  std::size_t occupied_{0};
  std::uint64_t gap_high_water_{0};
  std::uint64_t pending_end_sequence_{0};
  bool has_pending_end_{false};
  bool request_outstanding_{false};
};

}  // namespace qtrader::moldudp64
