#include "qtrader/DensePriceOrderBook.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

struct AuditSink
{
	std::vector<wthft::Trade> trades;
	std::vector<wthft::OrderUpdate> updates;
	void on_trade(const wthft::Trade& value) { trades.push_back(value); }
	void on_order(const wthft::OrderUpdate& value) { updates.push_back(value); }
	void clear() { trades.clear(); updates.clear(); }
};

bool equal(const AuditSink& left, const AuditSink& right)
{
	if (left.trades.size() != right.trades.size() || left.updates.size() != right.updates.size()) return false;
	for (std::size_t i = 0; i < left.trades.size(); ++i)
	{
		const auto& a = left.trades[i];
		const auto& b = right.trades[i];
		if (a.maker_id != b.maker_id || a.taker_id != b.taker_id ||
			a.price != b.price || a.quantity != b.quantity) return false;
	}
	for (std::size_t i = 0; i < left.updates.size(); ++i)
	{
		const auto& a = left.updates[i];
		const auto& b = right.updates[i];
		if (a.id != b.id || a.state != b.state || a.remaining != b.remaining) return false;
	}
	return true;
}

int main()
{
	{
		wthft::FixedOrderIndex index(2);
		bool index_ok = true;
		for (uint64_t cycle = 0; cycle < 10000 && index_ok; ++cycle)
		{
			const uint64_t first = cycle * 16 + 1;
			const uint64_t second = cycle * 16 + 9;
			index_ok = index.insert(first, 11) && index.insert(second, 22) &&
				index.find(first) == 11 && index.find(second) == 22 &&
				index.erase(first) && index.find(first) == wthft::FixedOrderIndex::invalid_index() &&
				index.find(second) == 22 && index.erase(second);
		}
		if (!index_ok)
		{
			std::fprintf(stderr, "fixed index churn/backshift failed\n");
			return 1;
		}
	}

	{
		wthft::DensePriceOrderBook feed(8, 100, 1, 20);
		bool feed_ok = feed.feed_add(1, wthft::Side::Buy, 105, 100) &&
			feed.feed_add(2, wthft::Side::Buy, 105, 50) &&
			feed.feed_add(3, wthft::Side::Sell, 110, 20);
		feed_ok = feed_ok && feed.best_bid() == 105 && feed.best_ask() == 110 &&
			feed.level_quantity(wthft::Side::Buy, 105) == 150;
		feed_ok = feed_ok && feed.feed_reduce(1, 40) && feed.remaining(1) == 60 &&
			feed.level_quantity(wthft::Side::Buy, 105) == 110;
		feed_ok = feed_ok && feed.feed_replace(1, 4, 106, 30) &&
			!feed.contains(1) && feed.contains(4) && feed.best_bid() == 106 &&
			feed.level_quantity(wthft::Side::Buy, 105) == 50 &&
			feed.level_quantity(wthft::Side::Buy, 106) == 30;
		feed_ok = feed_ok && feed.feed_delete(4) && feed.best_bid() == 105 &&
			feed.feed_reduce(2, 50) && feed.best_bid() == 0 &&
			feed.feed_delete(3) && feed.best_ask() == 0 && feed.active_orders() == 0;
		feed_ok = feed_ok && !feed.feed_delete(999) && !feed.feed_reduce(999, 1) &&
			!feed.feed_add(0, wthft::Side::Buy, 105, 1);
		if (!feed_ok)
		{
			std::fprintf(stderr, "direct market-data operations failed\n");
			return 1;
		}
	}

	constexpr std::size_t capacity = 250000;
	wthft::PriceTimeOrderBook reference(capacity);
	wthft::DensePriceOrderBook dense(capacity, 1, 1, 4096);
	AuditSink reference_sink;
	AuditSink dense_sink;
	std::mt19937_64 random(0xC0FFEE);
	std::vector<uint64_t> ids;
	ids.reserve(200000);
	uint64_t next_id = 1;

	bool ok = reinterpret_cast<std::uintptr_t>(dense.node_storage()) % 64 == 0;
	for (uint32_t operation = 0; operation < 200000 && ok; ++operation)
	{
		reference_sink.clear();
		dense_sink.clear();
		const uint32_t choice = static_cast<uint32_t>(random() % 100);
		bool reference_result = false;
		bool dense_result = false;

		if (choice < 68 || ids.empty())
		{
			const uint64_t id = next_id++;
			ids.push_back(id);
			const wthft::Side side = (random() & 1) ? wthft::Side::Buy : wthft::Side::Sell;
			const wthft::OrderType type = choice < 8 ? wthft::OrderType::Market : wthft::OrderType::Limit;
			const int64_t price = 950 + static_cast<int64_t>(random() % 101);
			const uint64_t quantity = 1 + random() % 20;
			const wthft::OrderRequest request{id, side, type, price, quantity};
			reference_result = reference.submit(request, reference_sink);
			dense_result = dense.submit(request, dense_sink);
		}
		else
		{
			const uint64_t id = ids[random() % ids.size()];
			if (choice < 84)
			{
				reference_result = reference.cancel(id, reference_sink);
				dense_result = dense.cancel(id, dense_sink);
			}
			else
			{
				const int64_t price = 950 + static_cast<int64_t>(random() % 101);
				const uint64_t quantity = 1 + random() % 20;
				reference_result = reference.modify(id, price, quantity, reference_sink);
				dense_result = dense.modify(id, price, quantity, dense_sink);
			}
		}

		ok = reference_result == dense_result && equal(reference_sink, dense_sink) &&
			reference.best_bid() == dense.best_bid() && reference.best_ask() == dense.best_ask() &&
			reference.active_orders() == dense.active_orders();
		if (!ok)
			std::fprintf(stderr, "dense/reference mismatch at operation %u\n", operation);
	}

	if (!ok) return 1;
	std::printf("DensePriceOrderBook: 64B_pool=ok operations=200000 reference_match=ok active=%zu\n",
		dense.active_orders());
	return 0;
}
