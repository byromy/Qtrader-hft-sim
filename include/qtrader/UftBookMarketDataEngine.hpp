#pragma once
#include "qtrader/UftMarketDataEngine.hpp"

namespace qtrader {
// Optional extension, leaving the original engine's layout/subscriptions intact.
// A single book stage owns each instrument; it fans out read-only strategy views.
template<std::size_t Instruments, std::size_t Strategies, std::size_t Subscribers = 8>
class UftBookMarketDataEngine : public UftMarketDataEngine<Instruments, Strategies, Subscribers> {
 public:
  using Callback = void (*)(void*, const UftBookUpdate&) noexcept;
  bool register_book(InstrumentId id, void* book, Callback callback) noexcept {
    if (this->configuration_frozen() || id >= Instruments || !book || !callback ||
        !this->instrument(id) || slots_[id].callback) return false;
    slots_[id] = {book, callback};
    return true;
  }
  std::size_t publish_book_update(const UftBookUpdate& event) const noexcept {
    if (!this->running() || event.instrument_id >= Instruments) return 0;
    const auto& slot = slots_[event.instrument_id];
    if (!slot.callback) return 0;
    slot.callback(slot.book, event);
    return 1;
  }
 private:
  struct Slot { void* book{nullptr}; Callback callback{nullptr}; };
  std::array<Slot, Instruments> slots_{};
};
}
