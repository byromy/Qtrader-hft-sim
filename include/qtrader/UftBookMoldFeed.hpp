#pragma once
#include "qtrader/MoldUdp64Recovery.hpp"
#include "qtrader/UftOrderBookPipeline.hpp"

namespace qtrader {
// Feed one complete received/recovered datagram at a time. No socket ownership.
// Bounded reordering and request generation are delegated to RecoveryWindow.
template<std::size_t Slots = 64, std::size_t PacketBytes = 2048>
class UftBookMoldFeed {
 public:
  UftBookMoldFeed(UftOrderBookPipeline& pipeline, std::string session)
      : pipeline_(pipeline), session_(std::move(session)) {
    if (session_.size() != moldudp64::kSessionBytes || session_ != pipeline_.session())
      throw std::invalid_argument("MoldUDP64 session must be exactly 10 bytes");
    moldudp64::Session id{};
    std::memcpy(id.data(), session_.data(), id.size());
    // Prime at the configured trusted boundary, not the first arriving packet:
    // otherwise losing the first packet could silently initialize past a gap.
    const auto seed = moldudp64::make_request_packet(id, pipeline_.next_sequence(), 0);
    recovery_.ingest(seed.data(), seed.size(),
      [](auto, auto, auto) {}, [](auto&, auto, auto) {}, [](auto, auto) {});
  }

  template<typename Request>
  moldudp64::RecoveryStatus ingest(const std::uint8_t* data, std::size_t size,
                                   Request&& request) {
    using Status = moldudp64::RecoveryStatus;
    const auto parsed = moldudp64::parse_downstream_packet(data, size,
      [](auto, auto, auto) noexcept {});
    if (!parsed) { pipeline_.invalidate(); return Status::malformed; }
    if (std::memcmp(parsed.header.session.data(), session_.data(), 10) != 0) {
      pipeline_.invalidate(); return Status::session_mismatch;
    }
    const auto& h = parsed.header;
    if (h.first_sequence > pipeline_.next_sequence()) {
      const auto last = h.kind() == moldudp64::PacketKind::data ?
          h.first_sequence + h.message_count - 1 : h.first_sequence - 1;
      pipeline_.note_gap(last);
    }
    const auto result = recovery_.ingest(data, size,
      [&](auto seq, auto message, auto length) {
        pipeline_.ingest(session_, seq, message, length);
      }, request,
      [&](auto kind, auto) {
        if (kind == moldudp64::PacketKind::end_of_session) pipeline_.invalidate();
      });
    if (result == Status::malformed || result == Status::session_mismatch ||
        result == Status::datagram_too_large || result == Status::reorder_window_full ||
        result == Status::sequence_overflow) pipeline_.invalidate();
    return result; // transport status; also check pipeline.state() for semantic faults
  }
 private:
  UftOrderBookPipeline& pipeline_;
  std::string session_;
  moldudp64::RecoveryWindow<Slots, PacketBytes> recovery_;
};
}
