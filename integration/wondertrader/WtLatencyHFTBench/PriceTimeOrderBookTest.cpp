#include "../WtHftComponents/PriceTimeOrderBook.hpp"

#include <cstdio>
#include <vector>

struct TestSink
{
	std::vector<wthft::Trade> trades;
	std::vector<wthft::OrderUpdate> updates;
	void on_trade(const wthft::Trade& trade) { trades.push_back(trade); }
	void on_order(const wthft::OrderUpdate& update) { updates.push_back(update); }
};

int main()
{
	wthft::PriceTimeOrderBook book(32);
	TestSink sink;
	using namespace wthft;
	bool ok = true;
	ok &= book.submit({1, Side::Sell, OrderType::Limit, 101, 5}, sink);
	ok &= book.submit({2, Side::Sell, OrderType::Limit, 101, 5}, sink);
	ok &= book.submit({3, Side::Buy, OrderType::Limit, 101, 7}, sink);
	ok &= sink.trades.size() == 2;
	ok &= sink.trades[0].maker_id == 1 && sink.trades[0].quantity == 5;
	ok &= sink.trades[1].maker_id == 2 && sink.trades[1].quantity == 2;
	ok &= book.best_ask() == 101 && book.active_orders() == 1;

	ok &= book.submit({4, Side::Buy, OrderType::Limit, 99, 4}, sink);
	ok &= book.submit({5, Side::Buy, OrderType::Limit, 99, 4}, sink);
	ok &= book.modify(4, 99, 4, sink);
	ok &= book.submit({6, Side::Sell, OrderType::Market, 0, 5}, sink);
	ok &= sink.trades[sink.trades.size() - 2].maker_id == 5;
	ok &= sink.trades.back().maker_id == 4;
	ok &= book.cancel(4, sink);
	ok &= !book.cancel(999, sink);

	if (!ok)
	{
		std::fprintf(stderr, "PriceTimeOrderBook test failed\n");
		return 1;
	}
	std::printf("PriceTimeOrderBook: price_priority=ok time_priority=ok partial_fill=ok modify=ok cancel=ok market=ok\n");
	return 0;
}
