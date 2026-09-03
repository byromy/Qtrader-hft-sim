#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/NasdaqItchUftAdapter.hpp"
#include "qtrader/NasdaqItchWtsAdapter.hpp"
#include "qtrader/UftMarketDataEngine.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
	using Engine = qtrader::UftMarketDataEngine<2, 2, 1>;

	std::string_view trimmed_symbol(const std::uint8_t* bytes)
	{
		std::size_t length = 8;
		while (length != 0 && bytes[length - 1] == ' ')
			--length;
		return {reinterpret_cast<const char*>(bytes), length};
	}

	std::uint32_t action_time(std::uint64_t timestamp_ns)
	{
		const auto milliseconds = timestamp_ns / 1'000'000ULL;
		return static_cast<std::uint32_t>(
			(milliseconds / 3'600'000ULL) * 10'000'000ULL +
			((milliseconds / 60'000ULL) % 60ULL) * 100'000ULL +
			((milliseconds / 1'000ULL) % 60ULL) * 1'000ULL +
			milliseconds % 1'000ULL);
	}

	qtrader::UftSide uft_side(std::uint32_t side)
	{
		return side == BDT_Buy ? qtrader::UftSide::Buy :
			side == BDT_Sell ? qtrader::UftSide::Sell : qtrader::UftSide::Unknown;
	}

	struct Comparison
	{
		wtp::WTSOrdDtlStruct expected_order{};
		wtp::WTSTransStruct expected_transaction{};
		bool have_order{false};
		bool have_transaction{false};
		std::uint64_t compared_orders{0};
		std::uint64_t compared_transactions{0};
		std::uint64_t mismatches{0};
		std::uint64_t ordering_errors{0};

		void on_order_detail(const wtp::WTSOrdDtlStruct& event)
		{
			if (have_order)
				++ordering_errors;
			expected_order = event;
			have_order = true;
		}

		void on_transaction(const wtp::WTSTransStruct& event)
		{
			if (have_transaction)
				++ordering_errors;
			expected_transaction = event;
			have_transaction = true;
		}

		void compare(const qtrader::UftOrderDetail& event)
		{
			++compared_orders;
			if (!have_order)
			{
				++ordering_errors;
				return;
			}
			const auto expected_ticks = static_cast<qtrader::PriceTicks>(
				std::llround(expected_order.price * 10'000.0));
			if (event.order_id != expected_order.index ||
				event.price_ticks != expected_ticks ||
				event.quantity != expected_order.volume ||
				event.side != uft_side(expected_order.side) ||
				event.order_type != qtrader::UftOrderType::Limit ||
				action_time(event.exchange_time_ns) != expected_order.action_time)
				++mismatches;
			have_order = false;
		}

		void compare(const qtrader::UftTransaction& event)
		{
			++compared_transactions;
			if (!have_transaction)
			{
				++ordering_errors;
				return;
			}
			const auto expected_ticks = static_cast<qtrader::PriceTicks>(
				std::llround(expected_transaction.price * 10'000.0));
			const auto expected_type = expected_transaction.ttype == TT_Match ?
				qtrader::UftTransactionType::Trade :
				expected_transaction.ttype == qtrader::itch50::kWtsTradeBust ?
					qtrader::UftTransactionType::TradeBust :
					qtrader::UftTransactionType::Cancel;
			if (event.transaction_id !=
					static_cast<std::uint64_t>(expected_transaction.index) ||
				event.price_ticks != expected_ticks ||
				event.quantity != expected_transaction.volume ||
				event.side != uft_side(expected_transaction.side) ||
				event.transaction_type != expected_type ||
				event.ask_order_id !=
					static_cast<std::uint64_t>(expected_transaction.askorder) ||
				event.bid_order_id !=
					static_cast<std::uint64_t>(expected_transaction.bidorder) ||
				action_time(event.exchange_time_ns) != expected_transaction.action_time)
				++mismatches;
			have_transaction = false;
		}

		void finish_message()
		{
			if (have_order || have_transaction)
				++ordering_errors;
			have_order = false;
			have_transaction = false;
		}
	};

	struct Strategy
	{
		Comparison* comparison{nullptr};

		static void init(void*, qtrader::UftStrategyContext& context) noexcept
		{
			(void)context.stra_sub_order_details(1);
			(void)context.stra_sub_transactions(1);
		}

		static void order(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftOrderDetail& event) noexcept
		{
			static_cast<Strategy*>(self)->comparison->compare(event);
		}

		static void transaction(void* self, const qtrader::UftStrategyContext&,
			const qtrader::UftTransaction& event) noexcept
		{
			static_cast<Strategy*>(self)->comparison->compare(event);
		}
	};

	int run(const char* path, std::string_view symbol, std::uint32_t trading_date,
		std::size_t order_capacity, std::size_t match_capacity)
	{
		Comparison comparison;
		Strategy strategy{&comparison};
		Engine engine;
		if (!engine.register_instrument(1, "PSX.TARGET", 1, 20'000'000, 1))
			throw std::runtime_error("failed to register instrument");
		qtrader::UftStrategyCallbacks callbacks{};
		callbacks.on_init = &Strategy::init;
		callbacks.on_order_detail = &Strategy::order;
		callbacks.on_transaction = &Strategy::transaction;
		if (!engine.register_strategy(1, &strategy, callbacks) || !engine.start())
			throw std::runtime_error("failed to start UFT engine");

		std::unique_ptr<qtrader::itch50::PsxWtsAdapter> reference;
		std::unique_ptr<qtrader::itch50::NasdaqItchUftAdapter> candidate;
		std::uint16_t locator = 0;
		std::uint64_t target_messages = 0;
		qtrader::itch50::scan_historical_file(path,
			[&](const std::uint8_t* message, std::size_t size)
			{
				if (message[0] == 'R' && size == 39 &&
					trimmed_symbol(message + 11) == symbol)
				{
					locator = qtrader::itch50::read_u16(message + 1);
					reference = std::make_unique<qtrader::itch50::PsxWtsAdapter>(
						trading_date, locator, symbol);
					candidate = std::make_unique<qtrader::itch50::NasdaqItchUftAdapter>(
						1, locator, order_capacity, match_capacity);
				}
				if (candidate != nullptr && size >= 11 &&
					qtrader::itch50::read_u16(message + 1) == locator)
				{
					++target_messages;
					reference->on_message(message, size, comparison);
					candidate->on_message(message, size, engine);
					comparison.finish_message();
				}
			});
		if (candidate == nullptr)
			throw std::runtime_error("symbol was not found");

		const auto& old_stats = reference->stats();
		const auto& new_stats = candidate->stats();
		const bool counts_equal =
			old_stats.order_details == new_stats.order_details &&
			old_stats.transactions == new_stats.transactions &&
			old_stats.matches == new_stats.matches &&
			old_stats.cancels == new_stats.cancels &&
			old_stats.trade_busts == new_stats.trade_busts &&
			old_stats.duplicate_matches_suppressed ==
				new_stats.duplicate_matches_suppressed &&
			old_stats.non_printable_suppressed ==
				new_stats.non_printable_suppressed &&
			old_stats.broken_trades_without_output ==
				new_stats.broken_trades_without_output &&
			old_stats.unknown_broken_trades == new_stats.unknown_broken_trades &&
			old_stats.errors == new_stats.malformed_or_missing_orders;

		std::cout << "Symbol: " << symbol << " locator=" << locator << '\n'
			<< "Target messages: " << target_messages << '\n'
			<< "Compared orders: " << comparison.compared_orders << '\n'
			<< "Compared transactions: " << comparison.compared_transactions << '\n'
			<< "Field mismatches: " << comparison.mismatches << '\n'
			<< "Ordering/count mismatches: " << comparison.ordering_errors << '\n'
			<< "Reference/candidate counts equal: "
			<< (counts_equal ? "yes" : "no") << '\n'
			<< "Candidate peak/end active orders: "
			<< new_stats.peak_active_orders << '/' << candidate->active_orders() << '\n'
			<< "Candidate capacity exhaustions: "
			<< new_stats.capacity_exhaustions << '\n'
			<< "Candidate startup table memory: " << std::fixed
			<< std::setprecision(2)
			<< static_cast<double>(candidate->startup_memory_bytes()) /
				(1024.0 * 1024.0) << " MiB\n";

		return counts_equal && comparison.mismatches == 0 &&
			comparison.ordering_errors == 0 &&
			new_stats.capacity_exhaustions == 0 && candidate->active_orders() == 0 ?
			0 : 3;
	}
}

int main(int argc, char** argv)
{
	if (argc != 6)
	{
		std::cerr << "Usage: " << argv[0]
			<< " ITCH_FILE SYMBOL YYYYMMDD ORDER_CAPACITY MATCH_CAPACITY\n";
		return 64;
	}
	try
	{
		const auto date = std::strtoull(argv[3], nullptr, 10);
		const auto orders = std::strtoull(argv[4], nullptr, 10);
		const auto matches = std::strtoull(argv[5], nullptr, 10);
		if (date == 0 || date > std::numeric_limits<std::uint32_t>::max() ||
			orders == 0 || matches == 0)
			throw std::runtime_error("invalid numeric argument");
		return run(argv[1], argv[2], static_cast<std::uint32_t>(date),
			static_cast<std::size_t>(orders), static_cast<std::size_t>(matches));
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << '\n';
		return 1;
	}
}
