#include "QUftOpaque.hpp"

#include "qtrader/NasdaqItchUftAdapter.hpp"
#include "qtrader/UftMarketDataEngine.hpp"

#include <memory>
#include <new>

namespace
{
	constexpr std::size_t kInstrumentCapacity = 16384;
	constexpr std::size_t kStrategyCapacity = 4096;
	constexpr std::size_t kStrategiesPerInstrument = 8;
	using Engine = qtrader::UftMarketDataEngine<kInstrumentCapacity,
		kStrategyCapacity, kStrategiesPerInstrument>;

#if defined(__GNUC__) || defined(__clang__)
#define QTRADER_NOINLINE __attribute__((noinline))
#else
#define QTRADER_NOINLINE
#endif

	struct CallbackBridge
	{
		void* strategy{nullptr};
		qtrader::InstrumentId instrument_id{qtrader::kInvalidInstrumentId};
		QUftOrderCallback order_callback{nullptr};
		QUftTransactionCallback transaction_callback{nullptr};

		static void init(void* self,
			qtrader::UftStrategyContext& context) noexcept
		{
			auto& bridge = *static_cast<CallbackBridge*>(self);
			(void)context.stra_sub_order_details(bridge.instrument_id);
			(void)context.stra_sub_transactions(bridge.instrument_id);
		}

		static void order(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftOrderDetail& event) noexcept
		{
			auto& bridge = *static_cast<CallbackBridge*>(self);
			bridge.order_callback(bridge.strategy, &event);
		}

		static void transaction(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftTransaction& event) noexcept
		{
			auto& bridge = *static_cast<CallbackBridge*>(self);
			bridge.transaction_callback(bridge.strategy, &event);
		}
	};
}

struct QUftOpaque
{
	Engine engine;
	CallbackBridge bridge;
	std::unique_ptr<qtrader::itch50::NasdaqItchUftAdapter> adapter;
};

QTRADER_NOINLINE QUftOpaque* quft_create(std::uint16_t locator,
	std::uint32_t instrument_id, std::uint32_t strategy_id,
	const char* standard_code, std::size_t maximum_active_orders,
	std::size_t maximum_unique_matches, void* strategy,
	QUftOrderCallback order_callback,
	QUftTransactionCallback transaction_callback) noexcept
{
	if (standard_code == nullptr || strategy == nullptr ||
		order_callback == nullptr || transaction_callback == nullptr)
		return nullptr;
	auto runner = std::unique_ptr<QUftOpaque>(new (std::nothrow) QUftOpaque());
	if (!runner)
		return nullptr;
	try
	{
		runner->bridge = {strategy, instrument_id, order_callback,
			transaction_callback};
		qtrader::UftStrategyCallbacks callbacks{};
		callbacks.on_init = &CallbackBridge::init;
		callbacks.on_order_detail = &CallbackBridge::order;
		callbacks.on_transaction = &CallbackBridge::transaction;
		if (!runner->engine.register_instrument(instrument_id, standard_code,
			1, 20'000'000, 1) ||
			!runner->engine.register_strategy(strategy_id, &runner->bridge,
				callbacks) || !runner->engine.start())
			return nullptr;
		runner->adapter =
			std::make_unique<qtrader::itch50::NasdaqItchUftAdapter>(
				instrument_id, locator, maximum_active_orders,
				maximum_unique_matches);
		return runner.release();
	}
	catch (...)
	{
		return nullptr;
	}
}

QTRADER_NOINLINE void quft_destroy(QUftOpaque* runner) noexcept
{
	delete runner;
}

QTRADER_NOINLINE void quft_message(QUftOpaque* runner,
	const std::uint8_t* message, std::size_t size) noexcept
{
	if (runner != nullptr && runner->adapter != nullptr)
		runner->adapter->on_message(message, size, runner->engine);
}

QTRADER_NOINLINE QUftOpaqueStats quft_stats(const QUftOpaque* runner) noexcept
{
	QUftOpaqueStats output{};
	if (runner == nullptr || runner->adapter == nullptr)
		return output;
	const auto& input = runner->adapter->stats();
	output.order_details = input.order_details;
	output.transactions = input.transactions;
	output.matches = input.matches;
	output.cancels = input.cancels;
	output.trade_busts = input.trade_busts;
	output.duplicate_matches_suppressed =
		input.duplicate_matches_suppressed;
	output.non_printable_suppressed = input.non_printable_suppressed;
	output.broken_trades_without_output = input.broken_trades_without_output;
	output.unknown_broken_trades = input.unknown_broken_trades;
	output.errors = input.malformed_or_missing_orders;
	output.capacity_exhaustions = input.capacity_exhaustions;
	output.peak_active_orders = input.peak_active_orders;
	output.order_events_delivered = input.order_events_delivered;
	output.transaction_events_delivered = input.transaction_events_delivered;
	output.callback_deliveries = input.callback_deliveries;
	output.undelivered_events = input.undelivered_events;
	return output;
}

QTRADER_NOINLINE std::size_t quft_active_orders(
	const QUftOpaque* runner) noexcept
{
	return runner == nullptr || runner->adapter == nullptr ? 0 :
		runner->adapter->active_orders();
}

QTRADER_NOINLINE std::size_t quft_startup_table_memory(
	const QUftOpaque* runner) noexcept
{
	return runner == nullptr || runner->adapter == nullptr ? 0 :
		runner->adapter->startup_memory_bytes();
}

std::size_t quft_runner_static_memory() noexcept
{
	// The adapter owns its two backing tables separately; those allocations are
	// reported by quft_startup_table_memory(). Include both control objects here.
	return sizeof(QUftOpaque) + sizeof(qtrader::itch50::NasdaqItchUftAdapter);
}

std::size_t quft_instrument_capacity() noexcept
{
	return kInstrumentCapacity;
}

std::size_t quft_strategy_capacity() noexcept
{
	return kStrategyCapacity;
}
