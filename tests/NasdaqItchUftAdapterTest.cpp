#include "qtrader/NasdaqItchUftAdapter.hpp"
#include "qtrader/UftMarketDataEngine.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace
{
	using Engine = qtrader::UftMarketDataEngine<2, 2, 2>;

	void put_u16(std::uint8_t* output, std::uint16_t value)
	{
		output[0] = static_cast<std::uint8_t>(value >> 8U);
		output[1] = static_cast<std::uint8_t>(value);
	}

	void put_u32(std::uint8_t* output, std::uint32_t value)
	{
		for (int index = 3; index >= 0; --index)
		{
			output[index] = static_cast<std::uint8_t>(value);
			value >>= 8U;
		}
	}

	void put_u48(std::uint8_t* output, std::uint64_t value)
	{
		for (int index = 5; index >= 0; --index)
		{
			output[index] = static_cast<std::uint8_t>(value);
			value >>= 8U;
		}
	}

	void put_u64(std::uint8_t* output, std::uint64_t value)
	{
		for (int index = 7; index >= 0; --index)
		{
			output[index] = static_cast<std::uint8_t>(value);
			value >>= 8U;
		}
	}

	template<std::size_t Size>
	std::array<std::uint8_t, Size> message(char type)
	{
		std::array<std::uint8_t, Size> result{};
		result[0] = static_cast<std::uint8_t>(type);
		put_u16(result.data() + 1, 7);
		put_u48(result.data() + 5, 123456789);
		return result;
	}

	struct Strategy
	{
		std::array<qtrader::UftOrderDetail, 4> orders{};
		std::array<qtrader::UftTransaction, 8> transactions{};
		std::size_t order_count{0};
		std::size_t transaction_count{0};

		static void init(void*, qtrader::UftStrategyContext& context) noexcept
		{
			assert(context.stra_sub_order_details(1));
			assert(context.stra_sub_transactions(1));
		}

		static void order(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftOrderDetail& event) noexcept
		{
			auto& strategy = *static_cast<Strategy*>(self);
			strategy.orders[strategy.order_count++] = event;
		}

		static void transaction(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftTransaction& event) noexcept
		{
			auto& strategy = *static_cast<Strategy*>(self);
			strategy.transactions[strategy.transaction_count++] = event;
		}
	};
}

int main()
{
	Engine engine;
	Strategy strategy;
	assert(engine.register_instrument(1, "PSX.TEST", 1, 10'000'000, 1));
	qtrader::UftStrategyCallbacks callbacks{};
	callbacks.on_init = &Strategy::init;
	callbacks.on_order_detail = &Strategy::order;
	callbacks.on_transaction = &Strategy::transaction;
	assert(engine.register_strategy(1, &strategy, callbacks));
	assert(engine.start());

	qtrader::itch50::NasdaqItchUftAdapter adapter(1, 7, 4, 8);

	auto add = message<36>('A');
	put_u64(add.data() + 11, 42);
	add[19] = 'B';
	put_u32(add.data() + 20, 100);
	put_u32(add.data() + 32, 250000);
	adapter.on_message(add.data(), add.size(), engine);

	auto cancel = message<23>('X');
	put_u64(cancel.data() + 11, 42);
	put_u32(cancel.data() + 19, 20);
	adapter.on_message(cancel.data(), cancel.size(), engine);

	auto execute = message<31>('E');
	put_u64(execute.data() + 11, 42);
	put_u32(execute.data() + 19, 30);
	put_u64(execute.data() + 23, 9);
	adapter.on_message(execute.data(), execute.size(), engine);

	auto non_printable = message<36>('C');
	put_u64(non_printable.data() + 11, 42);
	put_u32(non_printable.data() + 19, 10);
	put_u64(non_printable.data() + 23, 10);
	non_printable[31] = 'N';
	put_u32(non_printable.data() + 32, 255000);
	adapter.on_message(non_printable.data(), non_printable.size(), engine);
	auto broken_non_printable = message<19>('B');
	put_u64(broken_non_printable.data() + 11, 10);
	adapter.on_message(broken_non_printable.data(),
		broken_non_printable.size(), engine);

	auto replace = message<35>('U');
	put_u64(replace.data() + 11, 42);
	put_u64(replace.data() + 19, 43);
	put_u32(replace.data() + 27, 70);
	put_u32(replace.data() + 31, 260000);
	adapter.on_message(replace.data(), replace.size(), engine);

	auto deletion = message<19>('D');
	put_u64(deletion.data() + 11, 43);
	adapter.on_message(deletion.data(), deletion.size(), engine);

	auto trade = message<44>('P');
	put_u64(trade.data() + 11, 44);
	trade[19] = 'S';
	put_u32(trade.data() + 20, 5);
	put_u32(trade.data() + 32, 270000);
	put_u64(trade.data() + 36, 11);
	adapter.on_message(trade.data(), trade.size(), engine);
	adapter.on_message(trade.data(), trade.size(), engine);

	auto cross = message<40>('Q');
	put_u64(cross.data() + 11, 1000);
	put_u32(cross.data() + 27, 280000);
	put_u64(cross.data() + 31, 12);
	adapter.on_message(cross.data(), cross.size(), engine);

	auto broken = message<19>('B');
	put_u64(broken.data() + 11, 11);
	adapter.on_message(broken.data(), broken.size(), engine);
	// A second bust of the same match cannot be resolved after the first bust.
	adapter.on_message(broken.data(), broken.size(), engine);

	const auto& stats = adapter.stats();
	assert(adapter.active_orders() == 0);
	assert(stats.order_details == 2);
	assert(stats.transactions == 7);
	assert(stats.matches == 3);
	assert(stats.cancels == 3);
	assert(stats.trade_busts == 1);
	assert(stats.duplicate_matches_suppressed == 1);
	assert(stats.non_printable_suppressed == 1);
	assert(stats.broken_trades_without_output == 1);
	assert(stats.unknown_broken_trades == 1);
	assert(stats.malformed_or_missing_orders == 0);
	assert(stats.capacity_exhaustions == 0);
	assert(strategy.order_count == 2);
	assert(stats.order_events_delivered == 2);
	assert(stats.transaction_events_delivered == 7);
	assert(stats.callback_deliveries == 9);
	assert(stats.undelivered_events == 0);
	assert(strategy.transaction_count == 7);
	assert(strategy.orders[0].instrument_id == 1);
	assert(strategy.orders[0].order_id == 42);
	assert(strategy.orders[0].price_ticks == 250000);
	assert(strategy.orders[0].quantity == 100);
	assert(strategy.orders[1].order_id == 43);
	assert(strategy.orders[1].price_ticks == 260000);
	assert(strategy.transactions[0].transaction_type ==
		qtrader::UftTransactionType::Cancel);
	assert(strategy.transactions[1].transaction_id == 9);
	assert(strategy.transactions[1].transaction_type ==
		qtrader::UftTransactionType::Trade);
	assert(strategy.transactions[2].quantity == 40);
	assert(strategy.transactions[3].quantity == 70);
	assert(strategy.transactions[4].ask_order_id == 44);
	assert(strategy.transactions[5].side == qtrader::UftSide::Unknown);
	assert(strategy.transactions[6].transaction_id == 11);
	assert(strategy.transactions[6].transaction_type ==
		qtrader::UftTransactionType::TradeBust);

	struct NoDeliveryEngine
	{
		std::size_t publish_order_detail(const qtrader::UftOrderDetail&) noexcept
		{
			return 0;
		}
		std::size_t publish_transaction(const qtrader::UftTransaction&) noexcept
		{
			return 0;
		}
	} no_delivery;
	qtrader::itch50::NasdaqItchUftAdapter undelivered(1, 7, 4, 4);
	undelivered.on_message(add.data(), add.size(), no_delivery);
	assert(undelivered.stats().order_details == 1);
	assert(undelivered.stats().order_events_delivered == 0);
	assert(undelivered.stats().callback_deliveries == 0);
	assert(undelivered.stats().undelivered_events == 1);
	return 0;
}
