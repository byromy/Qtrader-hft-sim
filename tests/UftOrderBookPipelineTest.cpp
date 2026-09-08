#include "qtrader/UftBookMoldFeed.hpp"
#include <cassert>
#include <map>
#include <random>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>

using namespace qtrader;
using Bytes = std::vector<std::uint8_t>;
const std::string session = "BOOKTEST01";
void put(Bytes& b, unsigned offset, std::uint64_t n, unsigned width) {
  while (width) { b[offset + --width] = n & 255; n >>= 8; }
}
Bytes msg(char c) {
  Bytes b(itch50::expected_message_size(c)); b[0] = c;
  put(b, 1, 7, 2); put(b, 5, 123, 6); return b;
}
Bytes add(std::uint64_t id, unsigned price = 100, unsigned quantity = 10, char side = 'B', char type = 'A') {
  auto b = msg(type); put(b, 11, id, 8); b[19] = side;
  put(b, 20, quantity, 4); put(b, 32, price, 4); return b;
}
Bytes reduce(char c, std::uint64_t id, unsigned quantity = 0, std::uint64_t match = 0, bool printable = true) {
  auto b = msg(c); put(b, 11, id, 8);
  if (c != 'D') put(b, 19, quantity, 4);
  if (c == 'C' || c == 'E') put(b, 23, match, 8);
  if (c == 'C') { b[31] = printable ? 'Y' : 'N'; put(b, 32, 103, 4); }
  return b;
}
Bytes replace(std::uint64_t old_id, std::uint64_t new_id, unsigned price, unsigned qty) {
  auto b = msg('U'); put(b, 11, old_id, 8); put(b, 19, new_id, 8);
  put(b, 27, qty, 4); put(b, 31, price, 4); return b;
}
Bytes bust(std::uint64_t match) { auto b = msg('B'); put(b, 11, match, 8); return b; }
Bytes trade(char c, std::uint64_t match, unsigned qty) {
  auto b = msg(c);
  if (c == 'P') { b[19] = 'B'; put(b, 20, qty, 4); put(b, 32, 101, 4); put(b, 36, match, 8); }
  else { put(b, 11, qty, 8); put(b, 27, 101, 4); put(b, 31, match, 8); }
  return b;
}
struct Observer {
  unsigned calls{0}; std::uint64_t last_seq{0};
  static void callback(void* p, const UftOrderBookPipeline::Book& book,
      const UftBookUpdate* e, std::uint64_t seq, Quantity) noexcept {
    auto& self = *static_cast<Observer*>(p); ++self.calls; self.last_seq = seq;
    // Strategy sees the completed replace, never its intermediate state.
    if (e && e->action == UftBookAction::Replace) {
      assert(!book.contains(e->order_id));
      assert(book.remaining(e->new_order_id) == e->quantity);
    }
  }
};
BookIngestResult send(UftOrderBookPipeline& p, const Bytes& b) {
  return p.ingest(session, p.next_sequence(), b.data(), b.size());
}
void ok(UftOrderBookPipeline& p, const Bytes& b) { assert(send(p, b) == BookIngestResult::Applied); }
std::vector<std::uint64_t> fifo(const UftOrderBookPipeline& p, wthft::Side side, int price) {
  std::vector<std::uint64_t> out;
  p.book().visit_level(side, price, [&](auto id, auto) { out.push_back(id); }); return out;
}

void lifecycle() {
  UftOrderBookPipeline p(session, 1, 7, 32, 32, 90, 110, 1); Observer s;
  assert(p.start(&s, Observer::callback));
  ok(p, add(1)); ok(p, add(2)); ok(p, add(3, 105, 20, 'S', 'F'));
  assert((fifo(p, wthft::Side::Buy, 100) == std::vector<std::uint64_t>{1,2}));
  ok(p, reduce('X', 1, 3)); assert(p.book().remaining(1) == 7);
  ok(p, replace(1, 4, 100, 8));
  assert((fifo(p, wthft::Side::Buy, 100) == std::vector<std::uint64_t>{2,4}));
  ok(p, reduce('E', 2, 4, 1000)); assert(p.net_printed_volume() == 4);
  ok(p, reduce('C', 3, 5, 1001, false));
  assert(p.book().remaining(3) == 15 && p.net_printed_volume() == 4);
  ok(p, bust(1001)); assert(p.book().remaining(3) == 15);
  ok(p, bust(1000)); assert(p.book().remaining(2) == 6 && p.net_printed_volume() == 0);
  ok(p, reduce('C', 3, 15, 1002));
  assert(!p.book().contains(3));
  ok(p, bust(1002)); assert(!p.book().contains(3) && p.net_printed_volume() == 0);
  ok(p, trade('P', 2000, 7)); ok(p, trade('Q', 2001, 9));
  auto before = s.calls;
  ok(p, trade('P', 2000, 7)); // tape dedup, no depth mutation
  assert(s.calls == before && p.net_printed_volume() == 16);
  ok(p, bust(2000)); ok(p, bust(2001)); assert(p.net_printed_volume() == 0);
  ok(p, reduce('D', 2)); ok(p, reduce('X', 4, 8));
  assert(p.book().active_orders() == 0 && p.book().best_bid() == 0 && p.book().best_ask() == 0);
  // Market depth is not a matching simulator: crossed displayed orders coexist.
  ok(p, add(5, 108)); ok(p, add(6, 95, 10, 'S'));
  assert(p.book().active_orders() == 2);
}

void gaps() {
  UftOrderBookPipeline p(session, 1, 7, 16, 16, 90, 110, 1); Observer s;
  assert(p.start(&s, Observer::callback)); ok(p, add(1));
  auto future = add(3);
  assert(p.ingest(session, 3, future.data(), future.size()) == BookIngestResult::Gap);
  assert(!p.ready() && !p.book().contains(3) && s.calls == 1);
  ok(p, add(2)); assert(s.calls == 1 && !p.ready());
  ok(p, future); assert(p.ready() && s.calls == 2 && s.last_seq == 3);
  assert(p.ingest(session, 3, future.data(), future.size()) == BookIngestResult::Duplicate);
  assert(s.calls == 2 && p.book().active_orders() == 3);
  p.note_gap(5); ok(p, reduce('X', 1, 2)); assert(s.calls == 2);
  auto other = add(100); put(other, 1, 8, 2);
  ok(p, other); assert(p.ready() && s.calls == 3 && !p.book().contains(100));
  assert(p.ingest("OTHER", 6, future.data(), future.size()) == BookIngestResult::Faulted);
  assert(send(p, add(4)) == BookIngestResult::Faulted && s.calls == 3);
}

void faults() {
  for (unsigned scenario = 0; scenario < 13; ++scenario) {
    UftOrderBookPipeline p(session, 1, 7, 2, 1, 90, 110, 1); Observer s;
    assert(p.start(&s, Observer::callback)); ok(p, add(1));
    Bytes bad;
    switch (scenario) {
      case 0: bad = add(1); break;
      case 1: bad = reduce('X', 1, 11); break;
      case 2: bad = reduce('D', 999); break;
      case 3: bad = bust(999); break;
      case 4: bad = add(2, 111); break;
      case 5: ok(p, add(2)); bad = add(3); break;
      case 6: ok(p, reduce('E', 1, 1, 5)); bad = reduce('E', 1, 1, 6); break;
      case 7: bad = add(2); bad.pop_back(); break;
      case 8: ok(p, add(2)); bad = replace(1, 2, 100, 5); break;
      case 9: bad = reduce('C', 1, 1, 5); bad[31] = '?'; break;
      case 10: bad = trade('Q', 5, 0); break;
      case 11: ok(p, reduce('E', 1, 1, 5)); ok(p, bust(5)); bad = bust(5); break;
      default: ok(p, reduce('E', 1, 1, 5)); ok(p, bust(5)); bad = trade('P', 5, 1); break;
    }
    const auto calls = s.calls;
    assert(send(p, bad) == BookIngestResult::Faulted);
    assert(!p.ready() && p.error() && s.calls == calls);
    assert(send(p, reduce('D', 1)) == BookIngestResult::Faulted && s.calls == calls);
  }
}

// Independent std::map reference: compare ALL price levels, quantities, FIFO,
// active count and best prices after each generated raw ITCH transition.
void differential() {
  UftOrderBookPipeline p(session, 1, 7, 128, 20000, 90, 110, 1); Observer s;
  assert(p.start(&s, Observer::callback));
  struct Order { unsigned price, quantity; char side; std::uint64_t priority; };
  std::map<std::uint64_t, Order> ref;
  std::mt19937 rng(20260908); std::uint64_t id = 1, priority = 0, match = 1;
  for (unsigned step = 0; step < 10000; ++step) {
    auto choice = rng() % 5;
    if (ref.empty() || (choice == 0 && ref.size() < 100)) {
      unsigned price = 90 + rng() % 21, quantity = 1 + rng() % 100;
      char side = rng() & 1 ? 'B' : 'S';
      ok(p, add(id, price, quantity, side)); ref[id++] = {price, quantity, side, ++priority};
    } else {
      auto it = ref.begin(); std::advance(it, rng() % ref.size());
      const auto old = it->first;
      if (choice == 1) {
        unsigned price = 90 + rng() % 21, quantity = 1 + rng() % 100;
        const auto side = it->second.side;
        ok(p, replace(old, id, price, quantity)); ref.erase(it);
        ref[id++] = {price, quantity, side, ++priority};
      } else if (choice == 2) {
        ok(p, reduce('D', old)); ref.erase(it);
      } else {
        const auto qty = 1 + rng() % it->second.quantity;
        ok(p, reduce(choice == 3 ? 'X' : 'E', old, qty, match++));
        it->second.quantity -= qty; if (!it->second.quantity) ref.erase(it);
      }
    }
    assert(p.book().active_orders() == ref.size());
    int best_bid = 0, best_ask = 0;
    for (char side : {'B', 'S'}) for (int price = 90; price <= 110; ++price) {
      Quantity quantity = 0; std::vector<std::pair<std::uint64_t,std::uint64_t>> ordered;
      for (const auto& [key, o] : ref) if (o.side == side && o.price == unsigned(price)) {
        quantity += o.quantity; ordered.emplace_back(o.priority, key);
        assert(p.book().remaining(key) == o.quantity);
      }
      std::sort(ordered.begin(), ordered.end()); std::vector<std::uint64_t> ids;
      for (auto entry : ordered) ids.push_back(entry.second);
      auto bs = side == 'B' ? wthft::Side::Buy : wthft::Side::Sell;
      assert(p.book().level_quantity(bs, price) == quantity && fifo(p, bs, price) == ids);
      if (quantity && side == 'B') best_bid = price;
      if (quantity && side == 'S' && !best_ask) best_ask = price;
    }
    assert(p.book().best_bid() == best_bid && p.book().best_ask() == best_ask);
  }
}

Bytes packet(std::uint64_t seq, const Bytes& message) {
  Bytes b(22 + message.size()); std::copy(session.begin(), session.end(), b.begin());
  put(b, 10, seq, 8); put(b, 18, 1, 2); put(b, 20, message.size(), 2);
  std::copy(message.begin(), message.end(), b.begin() + 22); return b;
}
void mold_recovery() {
  UftOrderBookPipeline p(session, 1, 7, 16, 16, 90, 110, 1); Observer s;
  assert(p.start(&s, Observer::callback)); UftBookMoldFeed<2, 128> feed(p, session);
  unsigned requests = 0;
  auto request = [&](const auto&, auto seq, auto count) { ++requests; assert(seq >= 1 && count); };
  auto ingest = [&](unsigned seq, Bytes message) {
    auto b = packet(seq, message); return feed.ingest(b.data(), b.size(), request);
  };
  // Missing even the first packet is detected rather than silently accepted.
  assert(ingest(3, add(3)) == moldudp64::RecoveryStatus::buffered_gap);
  assert(s.calls == 0 && !p.ready() && requests == 1);
  ingest(1, add(1)); assert(s.calls == 0);
  ingest(2, add(2)); assert(s.calls == 1 && p.ready() && p.book().active_orders() == 3);
  ingest(2, add(2)); assert(s.calls == 1);
  ingest(5, reduce('D', 3)); ingest(4, replace(1, 4, 101, 3));
  assert(p.ready() && !p.book().contains(3) && p.book().contains(4));
  assert(s.calls == 2);
  ingest(7, add(7)); ingest(8, add(8));
  assert(ingest(9, add(9)) == moldudp64::RecoveryStatus::reorder_window_full);
  assert(!p.ready() && p.state() == BookFeedState::Faulted);
}
void channel_delivery() {
  UftBookMarketDataEngine<1,1,1> engine;
  assert(engine.register_instrument(0, "TEST", 90, 110, 1));
  assert(engine.start()); // Intentionally no book subscriber.
  itch50::NasdaqItchUftAdapter adapter(0, 7, 4, 4);
  const auto b = add(1);
  adapter.on_message(b.data(), b.size(), engine);
  assert(adapter.stats().book_events == 1);
  assert(adapter.stats().book_callback_deliveries == 0);
  assert(adapter.stats().undelivered_book_events == 1);
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--fixture") {
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc); assert(out);
    for (const auto& message : {add(1), add(2, 105, 20, 'S'), reduce('X', 1, 2),
         replace(1, 3, 101, 7), reduce('C', 2, 5, 1, false),
         reduce('E', 3, 2, 2), bust(2), reduce('D', 3)}) {
      const char length[2] = {0, static_cast<char>(message.size())};
      out.write(length, 2); out.write(reinterpret_cast<const char*>(message.data()), message.size());
    }
    out.close(); assert(out); return 0;
  }
  lifecycle(); gaps(); faults(); differential(); mold_recovery(); channel_delivery();
  std::cout << "UFT -> book -> strategy: lifecycle, gap recovery and 10000-transition differential PASS\n";
}
