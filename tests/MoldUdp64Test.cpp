#include "qtrader/MoldUdp64.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace mold = qtrader::moldudp64;

std::array<std::uint8_t, 27> two_message_packet(const mold::Session& session,
                                                std::uint64_t sequence) {
  std::array<std::uint8_t, 27> packet{};
  std::memcpy(packet.data(), session.data(), session.size());
  mold::encode_u64(packet.data() + 10, sequence);
  mold::encode_u16(packet.data() + 18, 2);
  mold::encode_u16(packet.data() + 20, 1);
  packet[22] = 'A';
  mold::encode_u16(packet.data() + 23, 2);
  packet[25] = 'B';
  packet[26] = 'C';
  return packet;
}

int main() {
  const mold::Session session{'P', 'S', 'X', '2', '0', '1', '9', '0', '7', '3'};
  const auto packet = two_message_packet(session, 100);

  std::uint64_t sequence_sum = 0;
  std::size_t bytes = 0;
  const auto parsed = mold::parse_downstream_packet(
      packet.data(), packet.size(),
      [&](std::uint64_t sequence, const std::uint8_t* data, std::size_t size) {
        sequence_sum += sequence;
        bytes += size;
        assert(data != nullptr);
      });
  assert(parsed);
  assert(parsed.header.session == session);
  assert(parsed.header.first_sequence == 100);
  assert(parsed.header.kind() == mold::PacketKind::data);
  assert(parsed.messages_visited == 2);
  assert(sequence_sum == 201);
  assert(bytes == 3);

  unsigned callbacks = 0;
  auto malformed = packet;
  malformed[24] = 10;  // Second body no longer fits.
  auto result = mold::parse_downstream_packet(
      malformed.data(), malformed.size(),
      [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++callbacks; });
  assert(result.error == mold::ParseError::message_truncated);
  assert(callbacks == 0);  // Full validation happens before side effects.

  result = mold::parse_downstream_packet(
      packet.data(), mold::kHeaderBytes - 1,
      [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++callbacks; });
  assert(result.error == mold::ParseError::header_truncated);

  auto trailing = packet;
  mold::encode_u16(trailing.data() + 18, 1);
  result = mold::parse_downstream_packet(
      trailing.data(), trailing.size(),
      [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++callbacks; });
  assert(result.error == mold::ParseError::trailing_bytes);

  std::array<std::uint8_t, mold::kHeaderBytes> heartbeat{};
  std::memcpy(heartbeat.data(), session.data(), session.size());
  mold::encode_u64(heartbeat.data() + 10, 102);
  result = mold::parse_downstream_packet(
      heartbeat.data(), heartbeat.size(),
      [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++callbacks; });
  assert(result && result.header.kind() == mold::PacketKind::heartbeat);

  auto end = heartbeat;
  mold::encode_u16(end.data() + 18, mold::kEndOfSession);
  result = mold::parse_downstream_packet(
      end.data(), end.size(),
      [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++callbacks; });
  assert(result && result.header.kind() == mold::PacketKind::end_of_session);

  mold::SequenceTracker tracker;
  auto decision = tracker.inspect(parsed.header);
  assert(decision.state == mold::SequenceState::in_order);
  tracker.commit(parsed.header, decision);
  assert(tracker.next_sequence() == 102);

  decision = tracker.inspect(parsed.header);
  assert(decision.state == mold::SequenceState::duplicate);
  assert(decision.skip_messages == 2);

  auto overlap_packet = two_message_packet(session, 101);
  mold::DownstreamHeader overlap_header;
  assert(mold::decode_header(overlap_packet.data(), overlap_packet.size(),
                             overlap_header));
  decision = tracker.inspect(overlap_header);
  assert(decision.state == mold::SequenceState::overlap);
  assert(decision.skip_messages == 1);
  tracker.commit(overlap_header, decision);
  assert(tracker.next_sequence() == 103);

  auto gap_packet = two_message_packet(session, 105);
  mold::DownstreamHeader gap_header;
  assert(mold::decode_header(gap_packet.data(), gap_packet.size(), gap_header));
  decision = tracker.inspect(gap_header);
  assert(decision.state == mold::SequenceState::gap);
  assert(decision.expected_sequence == 103);
  assert(decision.gap_messages == 2);
  tracker.commit(gap_header, decision);
  assert(tracker.next_sequence() == 103);

  auto other_session = session;
  other_session[0] = 'X';
  auto other_packet = two_message_packet(other_session, 103);
  mold::DownstreamHeader other_header;
  assert(mold::decode_header(other_packet.data(), other_packet.size(),
                             other_header));
  assert(tracker.inspect(other_header).state ==
         mold::SequenceState::session_mismatch);

  const auto request = mold::make_request_packet(session, 103, 2);
  assert(std::memcmp(request.data(), session.data(), session.size()) == 0);
  assert(qtrader::itch50::read_u64(request.data() + 10) == 103);
  assert(qtrader::itch50::read_u16(request.data() + 18) == 2);
  return 0;
}
