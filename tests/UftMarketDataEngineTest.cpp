#include "qtrader/UftMarketDataEngine.hpp"

#include <cassert>
#include <cstring>

namespace
{
	using Engine = qtrader::UftMarketDataEngine<4, 4, 2>;

	struct Strategy
	{
		qtrader::InstrumentId instrument_id{0};
		std::uint64_t sessions_started{0};
		std::uint64_t sessions_ended{0};
		std::uint64_t orders{0};
		std::uint64_t transactions{0};
		qtrader::PriceTicks price_sum{0};

		static void on_init(void* self,
			qtrader::UftStrategyContext& context) noexcept
		{
			auto& strategy = *static_cast<Strategy*>(self);
			assert(context.stra_sub_order_details(strategy.instrument_id));
			assert(context.stra_sub_transactions(strategy.instrument_id));
			const auto* instrument = context.instrument(strategy.instrument_id);
			assert(instrument != nullptr);
			assert(std::strcmp(instrument->standard_code.data(), "PSX.AAPL") == 0);
		}

		static void on_session_begin(void* self,
			const qtrader::UftStrategyContext&, std::uint32_t date) noexcept
		{
			assert(date == 20190730);
			++static_cast<Strategy*>(self)->sessions_started;
		}

		static void on_session_end(void* self,
			const qtrader::UftStrategyContext&, std::uint32_t date) noexcept
		{
			assert(date == 20190730);
			++static_cast<Strategy*>(self)->sessions_ended;
		}

		static void on_order(void* self, const qtrader::UftStrategyContext& context,
			const qtrader::UftOrderDetail& event) noexcept
		{
			auto& strategy = *static_cast<Strategy*>(self);
			assert(context.id() < 4);
			++strategy.orders;
			strategy.price_sum += event.price_ticks;
		}

		static void on_transaction(void* self,
			const qtrader::UftStrategyContext& context,
			const qtrader::UftTransaction& event) noexcept
		{
			auto& strategy = *static_cast<Strategy*>(self);
			assert(context.id() < 4);
			++strategy.transactions;
			strategy.price_sum += event.price_ticks;
		}
	};

	qtrader::UftStrategyCallbacks callbacks()
	{
		return {&Strategy::on_init, &Strategy::on_session_begin,
			&Strategy::on_session_end, &Strategy::on_order,
			&Strategy::on_transaction};
	}
}

int main()
{
	Engine engine;
	assert(engine.register_instrument(1, "PSX.AAPL", 1, 10000000, 1));
	assert(!engine.register_instrument(1, "PSX.AAPL", 1, 10000000, 1));
	assert(!engine.register_instrument(4, "PSX.BAD", 1, 2, 1));

	Strategy first{1};
	Strategy second{1};
	assert(engine.register_strategy(1, &first, callbacks()));
	assert(engine.register_strategy(2, &second, callbacks()));
	assert(!engine.register_strategy(2, &second, callbacks()));

	qtrader::UftOrderDetail order{};
	order.instrument_id = 1;
	order.order_id = 7;
	order.price_ticks = 10025;
	order.quantity = 10;
	order.side = qtrader::UftSide::Buy;
	order.order_type = qtrader::UftOrderType::Limit;
	assert(engine.publish_order_detail(order) == 0);

	assert(engine.start());
	assert(engine.running());
	assert(engine.configuration_frozen());
	assert(!engine.register_instrument(2, "PSX.MSFT", 1, 10000000, 1));
	assert(!engine.start());

	engine.on_session_begin(20190730);
	assert(engine.publish_order_detail(order) == 2);

	qtrader::UftTransaction transaction{};
	transaction.instrument_id = 1;
	transaction.transaction_id = 11;
	transaction.price_ticks = 10030;
	transaction.quantity = 3;
	transaction.transaction_type = qtrader::UftTransactionType::Trade;
	assert(engine.publish_transaction(transaction) == 2);
	engine.on_session_end(20190730);

	assert(first.sessions_started == 1 && second.sessions_started == 1);
	assert(first.sessions_ended == 1 && second.sessions_ended == 1);
	assert(first.orders == 1 && second.orders == 1);
	assert(first.transactions == 1 && second.transactions == 1);
	assert(first.price_sum == 20055 && second.price_sum == 20055);

	engine.stop();
	assert(engine.publish_order_detail(order) == 0);
	return 0;
}
