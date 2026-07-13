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
