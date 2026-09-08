#pragma once
#include "qtrader/NasdaqItchUftAdapter.hpp"
#include "qtrader/UftBookMarketDataEngine.hpp"
#include "qtrader/DensePriceOrderBook.hpp"
#include <limits>
#include <stdexcept>
#include <string>

namespace qtrader {

enum class BookFeedState { Live, Recovering, Faulted };
enum class BookIngestResult { Applied, Duplicate, Gap, Faulted };

// One instrument, one feed session, single-thread ownership. Start only from a
// known empty-book session boundary; a random mid-day first sequence is not a snapshot.
class UftOrderBookPipeline {
 public:
  using Book = wthft::DensePriceOrderBook;
  // A null update denotes recovery completion on a non-book source message.
  // References are callback-scoped; a strategy must not re-enter ingest().
  using Strategy = void (*)(void*, const Book&, const UftBookUpdate*,
                           std::uint64_t transport_sequence, Quantity net_printed_volume) noexcept;

  UftOrderBookPipeline(std::string session, std::uint64_t first_sequence,
      std::uint16_t locator, std::size_t max_orders, std::size_t max_matches,
      PriceTicks minimum, PriceTicks maximum, PriceTicks tick)
      : session_(std::move(session)), next_(first_sequence),
        adapter_(0, locator, checked_orders(max_orders), max_matches),
        book_(max_orders, minimum, tick, levels(minimum, maximum, tick)),
        matches_(max_matches) {
    if (session_.empty() || first_sequence == 0 || first_sequence == UINT64_MAX ||
        !engine_.register_instrument(0, "ITCH.BOOK", minimum, maximum, tick) ||
        !engine_.register_book(0, this, &receive))
      throw std::invalid_argument("invalid book feed configuration");
  }

  bool start(void* strategy, Strategy callback) noexcept {
    if (started_ || !strategy || !callback) return false;
    strategy_ = strategy;
    callback_ = callback;
    started_ = engine_.start();
    return started_;
  }

  // Supply EVERY message of this transport channel, including other symbols and
  // control ITCH messages. Future messages are not retained: replay them after
  // filling the gap. Never use the adapter-local source_sequence as this argument.
  BookIngestResult ingest(const std::string& session, std::uint64_t sequence,
                         const std::uint8_t* message, std::size_t size) noexcept {
    if (!started_ || in_ingest_ || state_ == BookFeedState::Faulted || session != session_)
      return fault("not started, reentrant, faulted or session mismatch");
    if (sequence < next_) return BookIngestResult::Duplicate;
    if (sequence == UINT64_MAX) return fault("sequence overflow");
    if (sequence > next_) {
      state_ = BookFeedState::Recovering;
      high_water_ = std::max(high_water_, sequence);
      return BookIngestResult::Gap;
    }
    if (!message || size < 11 || size != itch50::expected_message_size(message[0]))
      return fault("malformed or unsupported ITCH message");
    in_ingest_ = true;
    pending_ = false;
    const auto before = adapter_.stats();
    adapter_.on_message(message, size, engine_);
    const auto& after = adapter_.stats();
    if (after.malformed_or_missing_orders != before.malformed_or_missing_orders ||
        after.capacity_exhaustions != before.capacity_exhaustions ||
        after.unknown_broken_trades != before.unknown_broken_trades ||
        after.undelivered_book_events != before.undelivered_book_events ||
        book_.active_orders() != adapter_.active_orders())
      fault("adapter/book state error; rebuild from trusted session replay");
    if (state_ == BookFeedState::Faulted) {
      in_ingest_ = false;
      return BookIngestResult::Faulted;
    }
    ++next_;
    const bool recovered = state_ == BookFeedState::Recovering && next_ > high_water_;
    if (recovered) state_ = BookFeedState::Live;
    // Notify only after the complete raw message is applied and validated.
    // In particular a replace never exposes the transient cancel-only state.
    if (state_ == BookFeedState::Live && (pending_ || recovered)) {
      ++callbacks_;
      callback_(strategy_, book_, pending_ ? &last_ : nullptr, sequence, net_volume_);
    }
    in_ingest_ = false;
    return state_ == BookFeedState::Faulted ? BookIngestResult::Faulted : BookIngestResult::Applied;
  }

  // A heartbeat/feed controller can report a missing range without a data message.
  // highest_sequence is the last known missing/unapplied message, inclusive.
  void note_gap(std::uint64_t highest_sequence) noexcept {
    if (state_ == BookFeedState::Faulted || highest_sequence < next_) return;
    state_ = BookFeedState::Recovering;
    high_water_ = std::max(high_water_, highest_sequence);
  }
  void invalidate() noexcept { fault("external feed/session failure"); }
  BookFeedState state() const noexcept { return state_; }
  bool ready() const noexcept { return started_ && state_ == BookFeedState::Live; }
  const char* error() const noexcept { return error_; }
  std::uint64_t next_sequence() const noexcept { return next_; }
  const std::string& session() const noexcept { return session_; }
  std::uint64_t strategy_callbacks() const noexcept { return callbacks_; }
  Quantity net_printed_volume() const noexcept { return net_volume_; }
  const Book& book() const noexcept { return book_; } // diagnostics; check ready() first

 private:
  struct Match { Quantity quantity; bool printable; bool busted; };
  static std::size_t checked_orders(std::size_t n) {
    if (n == 0 || n >= UINT32_MAX) throw std::invalid_argument("invalid order capacity");
    return n;
  }
  static std::uint32_t levels(PriceTicks lo, PriceTicks hi, PriceTicks tick) {
    if (lo <= 0 || hi < lo || tick <= 0 || (hi - lo) % tick != 0 ||
        static_cast<std::uint64_t>((hi - lo) / tick) > UINT32_MAX - 64ULL)
      throw std::invalid_argument("invalid exchange price grid");
    return static_cast<std::uint32_t>((hi - lo) / tick + 1);
  }
  BookIngestResult fault(const char* reason) noexcept {
    state_ = BookFeedState::Faulted;
    if (!error_) error_ = reason;
    return BookIngestResult::Faulted;
  }
  static void receive(void* self, const UftBookUpdate& e) noexcept {
    static_cast<UftOrderBookPipeline*>(self)->apply(e);
  }
  void apply(const UftBookUpdate& e) noexcept {
    if (state_ == BookFeedState::Faulted) return;
    bool ok = e.quantity != 0;
    const auto side = e.side == UftSide::Buy ? wthft::Side::Buy : wthft::Side::Sell;
    switch (e.action) {
      case UftBookAction::Add:
        ok = ok && (e.side == UftSide::Buy || e.side == UftSide::Sell) &&
             book_.feed_add(e.order_id, side, e.price_ticks, e.quantity); break;
      case UftBookAction::Reduce:
        ok = ok && book_.feed_reduce(e.order_id, e.quantity); break;
      case UftBookAction::Delete:
        ok = ok && book_.remaining(e.order_id) == e.quantity && book_.feed_delete(e.order_id); break;
      case UftBookAction::Replace:
        ok = ok && book_.feed_replace(e.order_id, e.new_order_id, e.price_ticks, e.quantity); break;
      case UftBookAction::Execution:
        ok = ok && book_.feed_reduce(e.order_id, e.quantity);
        if (ok) ok = record(e);
        break;
      case UftBookAction::Trade: ok = ok && record(e); break;
      case UftBookAction::TradeBust: {
        auto* match = matches_.find(e.match_id);
        ok = match && !match->busted;
        if (ok) {
          if (match->printable) net_volume_ -= match->quantity;
          match->busted = true;
        }
        // Nasdaq B does NOT restore displayed orders or their time priority.
        break;
      }
    }
    if (!ok) { fault("invalid mutation or exhausted book/match capacity"); return; }
    last_ = e;
    pending_ = true;
  }
  bool record(const UftBookUpdate& e) noexcept {
    if (auto* match = matches_.find(e.match_id)) return !match->busted;
    if (e.printable && e.quantity > std::numeric_limits<Quantity>::max() - net_volume_) return false;
    if (matches_.insert(e.match_id, Match{e.quantity, e.printable, false}) != FixedHashInsertResult::Inserted)
      return false;
    if (e.printable) net_volume_ += e.quantity;
    return true;
  }
  std::string session_;
  std::uint64_t next_, high_water_{0}, callbacks_{0};
  itch50::NasdaqItchUftAdapter adapter_;
  Book book_;
  FixedHashTable<Match> matches_;
  UftBookMarketDataEngine<1, 1, 1> engine_;
  BookFeedState state_{BookFeedState::Live};
  const char* error_{nullptr};
  void* strategy_{nullptr};
  Strategy callback_{nullptr};
  UftBookUpdate last_{};
  Quantity net_volume_{0};
  bool pending_{false}, started_{false}, in_ingest_{false};
};
}
