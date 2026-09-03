#include <x86intrin.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct Payload {
  std::uint64_t sequence{0};
  std::array<std::uint8_t, 56> bytes{};
};
static_assert(sizeof(Payload) == 64, "benchmark datagram must stay 64 bytes");

class FileDescriptor {
 public:
  explicit FileDescriptor(int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const noexcept { return value_; }

 private:
  int value_;
};

inline std::uint64_t tsc_begin() noexcept {
  _mm_lfence();
  return __rdtsc();
}

inline std::uint64_t tsc_end() noexcept {
  unsigned int auxiliary = 0;
  const auto value = __rdtscp(&auxiliary);
  _mm_lfence();
  return value;
}

double estimate_tsc_hz() {
  const auto wall_begin = std::chrono::steady_clock::now();
  const auto cycle_begin = tsc_begin();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto cycle_end = tsc_end();
  const auto wall_end = std::chrono::steady_clock::now();
  return static_cast<double>(cycle_end - cycle_begin) /
         std::chrono::duration<double>(wall_end - wall_begin).count();
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                         double p) {
  const auto index = static_cast<std::size_t>(
      std::ceil(p * static_cast<double>(sorted.size())) - 1.0);
  return sorted[std::min(index, sorted.size() - 1)];
}

std::uint64_t measurement_overhead(std::uint32_t count) {
  std::vector<std::uint64_t> samples(count);
  for (auto& sample : samples) {
    const auto begin = tsc_begin();
    const auto end = tsc_end();
    sample = end - begin;
  }
  std::sort(samples.begin(), samples.end());
  return percentile(samples, 0.50);
}

void report(const char* name, std::vector<std::uint64_t> samples,
            std::uint64_t overhead, std::uint32_t batch, double tsc_hz) {
  for (auto& sample : samples) {
    sample = sample > overhead ? sample - overhead : 0;
  }
  std::sort(samples.begin(), samples.end());
  long double sum = 0;
  for (const auto sample : samples) {
    sum += sample;
  }
  const auto mean = static_cast<double>(sum / samples.size()) / batch;
  const auto p50 = static_cast<double>(percentile(samples, 0.50)) / batch;
  const auto p99 = static_cast<double>(percentile(samples, 0.99)) / batch;
  const auto p999 = static_cast<double>(percentile(samples, 0.999)) / batch;
  const auto ns_per_cycle = 1e9 / tsc_hz;
  std::printf(
      "%-24s mean=%8.2f ns p50=%8.2f ns p99=%8.2f ns "
      "p99.9=%9.2f ns\n",
      name, mean * ns_per_cycle, p50 * ns_per_cycle, p99 * ns_per_cycle,
      p999 * ns_per_cycle);
}

void send_exact(int socket, const Payload& payload) {
  const auto sent = ::send(socket, &payload, sizeof(payload), 0);
  if (sent != static_cast<ssize_t>(sizeof(payload))) {
    throw std::runtime_error("UDP loopback send failed");
  }
}

void receive_exact(int socket, Payload& payload) {
  const auto received = ::recv(socket, &payload, sizeof(payload), 0);
  if (received != static_cast<ssize_t>(sizeof(payload))) {
    throw std::runtime_error("UDP loopback receive failed");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto samples = argc > 1
                             ? static_cast<std::uint32_t>(
                                   std::strtoul(argv[1], nullptr, 10))
                             : 100'000U;
    const auto warmup = argc > 2
                            ? static_cast<std::uint32_t>(
                                  std::strtoul(argv[2], nullptr, 10))
                            : 10'000U;
    if (samples == 0) {
      return 2;
    }

    FileDescriptor receiver(::socket(AF_INET, SOCK_DGRAM, 0));
    FileDescriptor sender(::socket(AF_INET, SOCK_DGRAM, 0));
    if (receiver.get() < 0 || sender.get() < 0) {
      throw std::runtime_error("creating UDP sockets failed");
    }
    int receive_buffer = 8 * 1024 * 1024;
    ::setsockopt(receiver.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                 sizeof(receive_buffer));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(receiver.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
      throw std::runtime_error("binding UDP loopback receiver failed");
    }
    socklen_t address_length = sizeof(address);
    if (::getsockname(receiver.get(), reinterpret_cast<sockaddr*>(&address),
                      &address_length) != 0 ||
        ::connect(sender.get(), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
      throw std::runtime_error("connecting UDP loopback sender failed");
    }

    Payload outgoing{};
    Payload incoming{};
    for (std::uint32_t index = 0; index < warmup; ++index) {
      outgoing.sequence = index;
      send_exact(sender.get(), outgoing);
      receive_exact(receiver.get(), incoming);
      if (incoming.sequence != outgoing.sequence) {
        throw std::runtime_error("UDP sequence mismatch during warmup");
      }
    }

    std::vector<std::uint64_t> single(samples);
    for (std::uint32_t index = 0; index < samples; ++index) {
      outgoing.sequence = static_cast<std::uint64_t>(warmup) + index;
      send_exact(sender.get(), outgoing);
      const auto begin = tsc_begin();
      receive_exact(receiver.get(), incoming);
      const auto end = tsc_end();
      single[index] = end - begin;
      if (incoming.sequence != outgoing.sequence) {
        throw std::runtime_error("UDP sequence mismatch");
      }
    }

    constexpr std::uint32_t batch_size = 32;
    std::array<Payload, batch_size> outgoing_batch{};
    std::array<Payload, batch_size> incoming_batch{};
    std::array<iovec, batch_size> vectors{};
    std::array<mmsghdr, batch_size> messages{};
    for (std::uint32_t item = 0; item < batch_size; ++item) {
      vectors[item].iov_base = &incoming_batch[item];
      vectors[item].iov_len = sizeof(Payload);
      messages[item].msg_hdr.msg_iov = &vectors[item];
      messages[item].msg_hdr.msg_iovlen = 1;
    }

    std::vector<std::uint64_t> batched(samples);
    std::uint64_t next_sequence =
        static_cast<std::uint64_t>(warmup) + samples;
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
      for (std::uint32_t item = 0; item < batch_size; ++item) {
        outgoing_batch[item].sequence = next_sequence++;
        send_exact(sender.get(), outgoing_batch[item]);
        messages[item].msg_len = 0;
      }
      const auto begin = tsc_begin();
      const int received = ::recvmmsg(receiver.get(), messages.data(),
                                      batch_size, 0, nullptr);
      const auto end = tsc_end();
      if (received != static_cast<int>(batch_size)) {
        throw std::runtime_error("recvmmsg returned a partial batch");
      }
      batched[sample] = end - begin;
      for (std::uint32_t item = 0; item < batch_size; ++item) {
        if (messages[item].msg_len != sizeof(Payload) ||
            incoming_batch[item].sequence != outgoing_batch[item].sequence) {
          throw std::runtime_error("recvmmsg payload mismatch");
        }
      }
    }

    const auto overhead = measurement_overhead(100'000);
    const auto tsc_hz = estimate_tsc_hz();
    int actual_buffer = 0;
    socklen_t option_length = sizeof(actual_buffer);
    ::getsockopt(receiver.get(), SOL_SOCKET, SO_RCVBUF, &actual_buffer,
                 &option_length);
    std::printf(
        "scope=UDP_loopback_kernel_dequeue_not_physical_NIC payload=%zu "
        "samples=%u warmup=%u recvmmsg_batch=%u rcvbuf=%d "
        "median_timing_overhead=%llu_cycles tsc=%.3f_MHz\n",
        sizeof(Payload), samples, warmup, batch_size, actual_buffer,
        static_cast<unsigned long long>(overhead), tsc_hz / 1e6);
    report("recv_ready_1", std::move(single), overhead, 1, tsc_hz);
    report("recvmmsg_ready_32", std::move(batched), overhead, batch_size,
           tsc_hz);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
