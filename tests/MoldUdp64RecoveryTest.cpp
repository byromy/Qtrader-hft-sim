#include "qtrader/MoldUdp64Recovery.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mold = qtrader::moldudp64;

template <std::size_t N>
std::array<std::uint8_t, mold::kHeaderBytes + N * 3> make_packet(
    const mold::Session& session, const std::uint64_t sequence,
    const std::array<std::uint8_t, N>& values) {
  std::array<std::uint8_t, mold::kHeaderBytes + N * 3> packet{};
  std::memcpy(packet.data(), session.data(), session.size());
  mold::encode_u64(packet.data() + 10, sequence);
  mold::encode_u16(packet.data() + 18, static_cast<std::uint16_t>(N));
  std::size_t offset = mold::kHeaderBytes;
  for (const auto value : values) {
    mold::encode_u16(packet.data() + offset, 1);
    packet[offset + 2] = value;
    offset += 3;
  }
  return packet;
}

int main() {
  const mold::Session session{'P', 'S', 'X', 'R', 'E', 'C', 'O', 'V', '0', '1'};
  mold::RecoveryWindow<4, 64> recovery;
  std::vector<std::uint64_t> sequences;
  std::vector<std::uint8_t> values;
  struct Request { std::uint64_t sequence; std::uint16_t count; };
  std::vector<Request> requests;
  std::vector<mold::PacketKind> controls;

  auto on_message = [&](std::uint64_t sequence, const std::uint8_t* data,
                        std::size_t size) {
    assert(size == 1);
    sequences.push_back(sequence);
    values.push_back(data[0]);
  };
  auto on_request = [&](const mold::Session& requested_session,
                        std::uint64_t sequence, std::uint16_t count) {
    assert(requested_session == session);
    requests.push_back({sequence, count});
  };
  auto on_control = [&](mold::PacketKind kind, std::uint64_t) {
    controls.push_back(kind);
  };

  const auto first = make_packet(session, 100, std::array<std::uint8_t, 2>{1, 2});
  assert(recovery.ingest(first.data(), first.size(), on_message, on_request,
                         on_control) == mold::RecoveryStatus::processed);
  assert(recovery.next_sequence() == 102);

  const auto future = make_packet(session, 104, std::array<std::uint8_t, 2>{5, 6});
  assert(recovery.ingest(future.data(), future.size(), on_message, on_request,
                         on_control) == mold::RecoveryStatus::buffered_gap);
  assert(recovery.buffered_datagrams() == 1);
  assert(requests.size() == 1 && requests[0].sequence == 102 &&
         requests[0].count == 2);

  // A repeated future packet is recognized while it is still buffered.
  assert(recovery.ingest(future.data(), future.size(), on_message, on_request,
                         on_control) == mold::RecoveryStatus::duplicate);

  const auto recovery_one =
      make_packet(session, 102, std::array<std::uint8_t, 1>{3});
  assert(recovery.ingest(recovery_one.data(), recovery_one.size(), on_message,
                         on_request, on_control) ==
         mold::RecoveryStatus::processed);
  assert(recovery.next_sequence() == 103);
  assert(requests.size() == 2 && requests[1].sequence == 103 &&
         requests[1].count == 1);

  const auto recovery_two =
      make_packet(session, 103, std::array<std::uint8_t, 1>{4});
  assert(recovery.ingest(recovery_two.data(), recovery_two.size(), on_message,
                         on_request, on_control) ==
         mold::RecoveryStatus::processed);
  assert(recovery.next_sequence() == 106);
  assert(recovery.buffered_datagrams() == 0);
  assert((sequences == std::vector<std::uint64_t>{100, 101, 102, 103, 104, 105}));
  assert((values == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));

  std::array<std::uint8_t, mold::kHeaderBytes> heartbeat{};
  std::memcpy(heartbeat.data(), session.data(), session.size());
  mold::encode_u64(heartbeat.data() + 10, 106);
  assert(recovery.ingest(heartbeat.data(), heartbeat.size(), on_message,
                         on_request, on_control) ==
         mold::RecoveryStatus::heartbeat);
  assert(controls.back() == mold::PacketKind::heartbeat);

  auto end = heartbeat;
  mold::encode_u64(end.data() + 10, 108);
  mold::encode_u16(end.data() + 18, mold::kEndOfSession);
  assert(recovery.ingest(end.data(), end.size(), on_message, on_request,
                         on_control) == mold::RecoveryStatus::buffered_gap);
  assert(requests.back().sequence == 106 && requests.back().count == 2);

  const auto final_data =
      make_packet(session, 106, std::array<std::uint8_t, 2>{7, 8});
  assert(recovery.ingest(final_data.data(), final_data.size(), on_message,
                         on_request, on_control) ==
         mold::RecoveryStatus::processed);
  assert(recovery.next_sequence() == 108);
  assert(controls.back() == mold::PacketKind::end_of_session);

  auto malformed = first;
  malformed[21] = 10;
  const auto old_messages = sequences.size();
  assert(recovery.ingest(malformed.data(), malformed.size(), on_message,
                         on_request, on_control) ==
         mold::RecoveryStatus::malformed);
  assert(sequences.size() == old_messages);

  const auto& stats = recovery.stats();
  assert(stats.messages == 8);
  assert(stats.buffered_datagrams == 1);
  assert(stats.recovered_datagrams == 1);
  assert(stats.duplicates == 1);
  assert(stats.rerequests == 3);
  assert(stats.malformed == 1);
  return 0;
}
