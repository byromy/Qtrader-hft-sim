#include "qtrader/MoldUdp64.hpp"
#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchFile.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mold = qtrader::moldudp64;

namespace {

constexpr std::size_t kDatagramBytes = 1400;
struct MessageLimitReached {};

struct Fd {
  int value{-1};
  ~Fd() { if (value >= 0) ::close(value); }
};

struct ReceiverStats {
  std::uint64_t datagrams{0};
  std::uint64_t messages{0};
  std::uint64_t bytes{0};
  std::uint64_t malformed_packets{0};
  std::uint64_t malformed_itch{0};
  std::uint64_t gaps{0};
  std::uint64_t gap_messages{0};
  std::uint64_t duplicates{0};
  std::uint64_t session_mismatches{0};
  bool end_of_session{false};
  double seconds{0};
};

void set_receive_timeout(const int fd) {
  timeval timeout{};
  timeout.tv_usec = 100000;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
    throw std::runtime_error(std::string("SO_RCVTIMEO failed: ") +
                             std::strerror(errno));
  }
}

ReceiverStats receive_packets(const int fd, const std::atomic<bool>& sender_done) {
  ReceiverStats stats;
  mold::SequenceTracker tracker;
  std::vector<std::uint8_t> buffer(65536);
  bool started = false;
  std::chrono::steady_clock::time_point start;
  auto finish = std::chrono::steady_clock::now();

  for (;;) {
    const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && sender_done.load()) break;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
    }
    if (!started) {
      start = std::chrono::steady_clock::now();
      started = true;
    }
    finish = std::chrono::steady_clock::now();
    ++stats.datagrams;
    stats.bytes += static_cast<std::uint64_t>(received);

    mold::DownstreamHeader header;
    if (!mold::decode_header(buffer.data(), static_cast<std::size_t>(received),
                             header)) {
      ++stats.malformed_packets;
      continue;
    }
    const auto decision = tracker.inspect(header);
    if (decision.state == mold::SequenceState::gap) {
      ++stats.gaps;
      stats.gap_messages += decision.gap_messages;
    } else if (decision.state == mold::SequenceState::duplicate) {
      ++stats.duplicates;
    } else if (decision.state == mold::SequenceState::session_mismatch) {
      ++stats.session_mismatches;
    }

    std::uint16_t message_index = 0;
    const auto parsed = mold::parse_downstream_packet(
        buffer.data(), static_cast<std::size_t>(received),
        [&](std::uint64_t, const std::uint8_t* message, std::size_t size) {
          const bool process = decision.may_process() &&
                               message_index >= decision.skip_messages;
          ++message_index;
          if (!process) return;
          ++stats.messages;
          const auto expected = size == 0 ? 0 :
              qtrader::itch50::expected_message_size(message[0]);
          if (expected == 0 || expected != size) ++stats.malformed_itch;
        });
    if (!parsed) {
      ++stats.malformed_packets;
      continue;
    }
    tracker.commit(header, decision);
    if (header.kind() == mold::PacketKind::end_of_session) {
      stats.end_of_session = true;
      break;
    }
  }
  if (started) stats.seconds = std::chrono::duration<double>(finish - start).count();
  return stats;
}

std::uint64_t parse_limit(const char* text) {
  const auto value = std::stoull(text);
  if (value == 0) throw std::runtime_error("message limit must be positive");
  return value;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: " << argv[0] << " ITCH_FILE [MESSAGE_LIMIT]\n";
    return 2;
  }
  const std::uint64_t limit = argc == 3 ? parse_limit(argv[2]) : 1'000'000;
  const mold::Session session{'P', 'S', 'X', '2', '0', '1', '9', '0', '7', '3'};

  Fd receiver{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
  Fd sender{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
  if (receiver.value < 0 || sender.value < 0) {
    throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
  }
  int receive_bytes = 16 * 1024 * 1024;
  (void)::setsockopt(receiver.value, SOL_SOCKET, SO_RCVBUF, &receive_bytes,
                     sizeof(receive_bytes));
  set_receive_timeout(receiver.value);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(receiver.value, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
  }
  socklen_t address_size = sizeof(address);
  if (::getsockname(receiver.value, reinterpret_cast<sockaddr*>(&address),
                    &address_size) != 0) {
    throw std::runtime_error(std::string("getsockname failed: ") +
                             std::strerror(errno));
  }

  std::atomic<bool> sender_done{false};
  ReceiverStats receiver_stats;
  std::exception_ptr receiver_error;
  std::thread receiver_thread([&] {
    try {
      receiver_stats = receive_packets(receiver.value, sender_done);
    } catch (...) {
      receiver_error = std::current_exception();
    }
  });

  std::vector<std::uint8_t> datagram(mold::kHeaderBytes, 0);
  std::memcpy(datagram.data(), session.data(), session.size());
  std::uint64_t first_sequence = 1;
  std::uint16_t count = 0;
  std::uint64_t sent_messages = 0;
  std::uint64_t sent_datagrams = 0;
  const auto sender_start = std::chrono::steady_clock::now();

  auto send_datagram = [&] {
    if (count == 0) return;
    mold::encode_u64(datagram.data() + 10, first_sequence);
    mold::encode_u16(datagram.data() + 18, count);
    ssize_t sent;
    do {
      sent = ::sendto(sender.value, datagram.data(), datagram.size(), 0,
                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(datagram.size())) {
      throw std::runtime_error(std::string("sendto failed: ") +
                               std::strerror(errno));
    }
    first_sequence += count;
    ++sent_datagrams;
    count = 0;
    datagram.resize(mold::kHeaderBytes);
  };

  try {
    try {
      qtrader::itch50::scan_historical_file(
          argv[1], [&](const std::uint8_t* message, std::uint16_t size) {
            if (sent_messages >= limit) throw MessageLimitReached{};
            const auto block_size = static_cast<std::size_t>(size) + 2;
            if (datagram.size() + block_size > kDatagramBytes) send_datagram();
            const auto old_size = datagram.size();
            datagram.resize(old_size + block_size);
            mold::encode_u16(datagram.data() + old_size, size);
            std::memcpy(datagram.data() + old_size + 2, message, size);
            ++count;
            ++sent_messages;
          });
    } catch (const MessageLimitReached&) {
      // The benchmark intentionally reads only the requested prefix.
    }
    send_datagram();

    auto end = mold::make_request_packet(session, first_sequence,
                                         mold::kEndOfSession);
    const auto sent = ::sendto(sender.value, end.data(), end.size(), 0,
                               reinterpret_cast<const sockaddr*>(&address),
                               sizeof(address));
    if (sent != static_cast<ssize_t>(end.size())) {
      throw std::runtime_error(std::string("end sendto failed: ") +
                               std::strerror(errno));
    }
  } catch (...) {
    sender_done.store(true);
    receiver_thread.join();
    throw;
  }
  const auto sender_finish = std::chrono::steady_clock::now();
  sender_done.store(true);
  receiver_thread.join();
  if (receiver_error) std::rethrow_exception(receiver_error);

  const auto sender_seconds =
      std::chrono::duration<double>(sender_finish - sender_start).count();
  std::cout << "Historical messages packetized: " << sent_messages << "\n"
            << "MoldUDP64 data datagrams sent: " << sent_datagrams << "\n"
            << "Receiver datagrams (including EOS): "
            << receiver_stats.datagrams << "\n"
            << "Receiver messages: " << receiver_stats.messages << "\n"
            << "Malformed MoldUDP64 / ITCH: " << receiver_stats.malformed_packets
            << " / " << receiver_stats.malformed_itch << "\n"
            << "Sequence gaps / missing messages: " << receiver_stats.gaps
            << " / " << receiver_stats.gap_messages << "\n"
            << "Duplicates / session mismatches: " << receiver_stats.duplicates
            << " / " << receiver_stats.session_mismatches << "\n"
            << "End of session received: "
            << (receiver_stats.end_of_session ? "yes" : "no") << "\n"
            << std::fixed << std::setprecision(3)
            << "Sender file+packet+send rate: "
            << static_cast<double>(sent_messages) / sender_seconds / 1e6
            << " Mmsg/s\n"
            << "Receiver UDP+Mold+ITCH rate: "
            << (receiver_stats.seconds == 0 ? 0.0 :
                static_cast<double>(receiver_stats.messages) /
                    receiver_stats.seconds / 1e6)
            << " Mmsg/s\n";

  return receiver_stats.messages == sent_messages &&
                 receiver_stats.malformed_packets == 0 &&
                 receiver_stats.malformed_itch == 0 &&
                 receiver_stats.gaps == 0 && receiver_stats.end_of_session
             ? 0
             : 1;
} catch (const std::exception& error) {
  std::cerr << "error: " << error.what() << '\n';
  return 1;
}
