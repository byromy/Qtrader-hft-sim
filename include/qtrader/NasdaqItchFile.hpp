#pragma once

#include "qtrader/NasdaqItch50.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace qtrader::itch50 {

struct FileScanResult {
  std::uint64_t messages{0};
  std::uint64_t framed_bytes{0};
};

namespace detail {

class BufferedFile {
 public:
  static constexpr std::size_t kBufferBytes = 8U * 1024U * 1024U;

  explicit BufferedFile(const char* path) : buffer_(kBufferBytes) {
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      throw std::runtime_error(std::string("open failed: ") + std::strerror(errno));
    }
#if defined(POSIX_FADV_SEQUENTIAL)
    (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }

  BufferedFile(const BufferedFile&) = delete;
  BufferedFile& operator=(const BufferedFile&) = delete;

  ~BufferedFile() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool ensure(const std::size_t requested) {
    if (requested > buffer_.size()) {
      throw std::runtime_error("record is larger than the read buffer");
    }
    if (end_ - begin_ >= requested) {
      return true;
    }
    if (begin_ != 0 && begin_ != end_) {
      std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
      end_ -= begin_;
      begin_ = 0;
    } else if (begin_ == end_) {
      begin_ = 0;
      end_ = 0;
    }

    while (end_ < requested && !eof_) {
      const auto capacity = buffer_.size() - end_;
      const auto count = ::read(fd_, buffer_.data() + end_, capacity);
      if (count > 0) {
        end_ += static_cast<std::size_t>(count);
      } else if (count == 0) {
        eof_ = true;
      } else if (errno != EINTR) {
        throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
      }
    }
    return end_ - begin_ >= requested;
  }

  const std::uint8_t* data() const noexcept { return buffer_.data() + begin_; }
  void consume(const std::size_t count) noexcept { begin_ += count; }
  std::size_t available() const noexcept { return end_ - begin_; }

 private:
  int fd_{-1};
  std::vector<std::uint8_t> buffer_;
  std::size_t begin_{0};
  std::size_t end_{0};
  bool eof_{false};
};

}  // namespace detail

template <typename Callback>
FileScanResult scan_historical_file(const char* path, Callback&& callback) {
  detail::BufferedFile input(path);
  FileScanResult result;
  while (input.ensure(2)) {
    const auto record_offset = result.framed_bytes;
    const auto record_size = read_u16(input.data());
    input.consume(2);
    result.framed_bytes += 2;
    if (record_size == 0) {
      throw std::runtime_error("zero-length record at framed byte " +
                               std::to_string(record_offset));
    }
    if (!input.ensure(record_size)) {
      throw std::runtime_error("truncated record at framed byte " +
                               std::to_string(record_offset) + "; wanted " +
                               std::to_string(record_size) + " bytes, got " +
                               std::to_string(input.available()));
    }
    std::forward<Callback>(callback)(input.data(), record_size);
    input.consume(record_size);
    result.framed_bytes += record_size;
    ++result.messages;
  }
  if (input.available() != 0) {
    throw std::runtime_error("trailing byte after final record");
  }
  return result;
}

}  // namespace qtrader::itch50
