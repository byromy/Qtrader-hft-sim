#pragma once

#include "qtrader/FixedHashTable.hpp"
#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/UftTypes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace qtrader::itch50
{
	struct UftAdapterStats
	{
		std::uint64_t order_details{0};
		std::uint64_t transactions{0};
		std::uint64_t matches{0};
		std::uint64_t cancels{0};
		std::uint64_t trade_busts{0};
		std::uint64_t duplicate_matches_suppressed{0};
		std::uint64_t non_printable_suppressed{0};
		std::uint64_t broken_trades_without_output{0};
		std::uint64_t unknown_broken_trades{0};
		std::uint64_t malformed_or_missing_orders{0};
		std::uint64_t capacity_exhaustions{0};
		std::uint64_t peak_active_orders{0};
		std::uint64_t order_events_delivered{0};
		std::uint64_t transaction_events_delivered{0};
		std::uint64_t callback_deliveries{0};
		std::uint64_t undelivered_events{0};
		std::uint64_t book_events{0};
		std::uint64_t book_callback_deliveries{0};
		std::uint64_t undelivered_book_events{0};
	};

	// One instrument per adapter keeps the packet hot path free of symbol lookup.
	// A startup directory maps stock_locate to one of these adapters.
	class NasdaqItchUftAdapter
	{
		struct Order
		{
			PriceTicks price_ticks{0};
			Quantity remaining{0};
			UftSide side{UftSide::Unknown};
		};

		struct Match
		{
			PriceTicks price_ticks{0};
			Quantity quantity{0};
			std::uint64_t order_id{0};
			UftSide side{UftSide::Unknown};
			bool emitted{false};
		};

	public:
		NasdaqItchUftAdapter(InstrumentId instrument_id, std::uint16_t locator,
			std::size_t maximum_active_orders,
			std::size_t maximum_unique_matches)
			: instrument_id_(instrument_id), locator_(locator),
			  orders_(maximum_active_orders),
			  matches_(maximum_unique_matches)
		{
		}

		template<typename Engine>
		void on_message(const std::uint8_t* message, std::size_t size,
			Engine& engine) noexcept
		{
			if (message == nullptr || size < 11 ||
				read_u16(message + 1) != locator_)
				return;
			++source_sequence_;
			if (size != expected_message_size(message[0]))
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			switch (message[0])
			{
			case 'A':
			case 'F': add(message, engine); break;
			case 'E':
			case 'C': execute(message, engine); break;
			case 'X': cancel_partial(message, engine); break;
			case 'D': cancel_all(message, engine); break;
			case 'U': replace(message, engine); break;
			case 'P': non_cross_trade(message, engine); break;
			case 'Q': cross_trade(message, engine); break;
			case 'B': broken_trade(message, engine); break;
			default: break;
			}
		}

		const UftAdapterStats& stats() const noexcept { return stats_; }
		std::size_t active_orders() const noexcept { return orders_.size(); }
		std::size_t startup_memory_bytes() const noexcept
		{
			return orders_.allocated_bytes() + matches_.allocated_bytes();
		}

	private:
		// Optional channel: existing UFT-only engines compile this out.
		template<typename Engine>
		auto book_publish(Engine& engine, const UftBookUpdate& event, int) noexcept
			-> decltype(engine.publish_book_update(event), void())
		{
			++stats_.book_events;
			const auto delivered = engine.publish_book_update(event);
			stats_.book_callback_deliveries += delivered;
			if (!delivered) ++stats_.undelivered_book_events;
		}
		template<typename Engine>
		static void book_publish(Engine&, const UftBookUpdate&, long) noexcept {}

		template<typename Engine>
		void emit_book(const std::uint8_t* message, UftBookAction action,
			std::uint64_t id, std::uint64_t new_id, std::uint64_t match_id,
			PriceTicks price, Quantity quantity, UftSide side, bool printable,
			Engine& engine) noexcept
		{
			book_publish(engine, UftBookUpdate{source_sequence_, read_u48(message + 5),
				id, new_id, match_id, price, quantity, instrument_id_, side, action,
				printable}, 0);
		}

		static UftSide side_of(std::uint8_t side) noexcept
		{
			return side == 'B' ? UftSide::Buy :
				side == 'S' ? UftSide::Sell : UftSide::Unknown;
		}

		template<typename Engine>
		void emit_order(const std::uint8_t* message, std::uint64_t order_id,
			const Order& order, Engine& engine) noexcept
		{
			UftOrderDetail event{};
			event.source_sequence = source_sequence_;
			event.exchange_time_ns = read_u48(message + 5);
			event.order_id = order_id;
			event.price_ticks = order.price_ticks;
			event.quantity = order.remaining;
			event.instrument_id = instrument_id_;
			event.side = order.side;
			event.order_type = UftOrderType::Limit;
			++stats_.order_details;
			const auto deliveries = engine.publish_order_detail(event);
			stats_.callback_deliveries += deliveries;
			if (deliveries == 0)
				++stats_.undelivered_events;
			else
				++stats_.order_events_delivered;
		}

		template<typename Engine>
		void emit_transaction(const std::uint8_t* message,
			std::uint64_t transaction_id, UftTransactionType type, UftSide side,
			PriceTicks price_ticks, Quantity quantity, std::uint64_t order_id,
			Engine& engine) noexcept
		{
			if (quantity == 0)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			UftTransaction event{};
			event.source_sequence = source_sequence_;
			event.exchange_time_ns = read_u48(message + 5);
			event.transaction_id = transaction_id;
			event.ask_order_id = side == UftSide::Sell ? order_id : 0;
			event.bid_order_id = side == UftSide::Buy ? order_id : 0;
			event.price_ticks = price_ticks;
			event.quantity = quantity;
			event.instrument_id = instrument_id_;
			event.side = side;
			event.transaction_type = type;
			++stats_.transactions;
			const auto deliveries = engine.publish_transaction(event);
			stats_.callback_deliveries += deliveries;
			if (deliveries == 0)
				++stats_.undelivered_events;
			else
				++stats_.transaction_events_delivered;
			if (type == UftTransactionType::Trade)
				++stats_.matches;
			else if (type == UftTransactionType::Cancel)
				++stats_.cancels;
			else if (type == UftTransactionType::TradeBust)
				++stats_.trade_busts;
		}

		void count_insert_failure(FixedHashInsertResult result) noexcept
		{
			if (result == FixedHashInsertResult::Full)
				++stats_.capacity_exhaustions;
			else
				++stats_.malformed_or_missing_orders;
		}

		template<typename Engine>
		void add(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto order_id = read_u64(message + 11);
			const auto side = side_of(message[19]);
			const auto quantity = read_u32(message + 20);
			const auto price = read_u32(message + 32);
			if (side == UftSide::Unknown || quantity == 0 || price == 0)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const Order order{static_cast<PriceTicks>(price), quantity, side};
			const auto inserted = orders_.insert(order_id, order);
			if (inserted != FixedHashInsertResult::Inserted)
			{
				count_insert_failure(inserted);
				return;
			}
			stats_.peak_active_orders = std::max(stats_.peak_active_orders,
				static_cast<std::uint64_t>(orders_.size()));
			emit_order(message, order_id, order, engine);
			emit_book(message, UftBookAction::Add, order_id, 0, 0,
				order.price_ticks, quantity, side, false, engine);
		}

		template<typename Engine>
		void execute(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto order_id = read_u64(message + 11);
			const auto quantity = read_u32(message + 19);
			auto* order = orders_.find(order_id);
			if (order == nullptr || quantity == 0 || quantity > order->remaining)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const auto match_id = read_u64(message + 23);
			const auto price = message[0] == 'C' ?
				static_cast<PriceTicks>(read_u32(message + 32)) : order->price_ticks;
			const bool printable = message[0] != 'C' || message[31] == 'Y';
			if (price <= 0 || (message[0] == 'C' && message[31] != 'Y' && message[31] != 'N'))
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const Match match{price, quantity, order_id, order->side, printable};
			const auto first = matches_.insert(match_id, match);
			if (first == FixedHashInsertResult::Full || first == FixedHashInsertResult::InvalidKey)
			{
				count_insert_failure(first);
				return;
			}
			if (!printable)
				++stats_.non_printable_suppressed;
			else if (first == FixedHashInsertResult::Inserted)
				emit_transaction(message, match_id, UftTransactionType::Trade,
					order->side, price, quantity, order_id, engine);
			else if (first == FixedHashInsertResult::Duplicate)
				++stats_.duplicate_matches_suppressed;
			else
				count_insert_failure(first);
			emit_book(message, UftBookAction::Execution, order_id, 0, match_id,
				price, quantity, order->side, printable, engine);
			order->remaining -= quantity;
			if (order->remaining == 0)
				orders_.erase(order_id);
		}

		template<typename Engine>
		void cancel_partial(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto order_id = read_u64(message + 11);
			const auto quantity = read_u32(message + 19);
			auto* order = orders_.find(order_id);
			if (order == nullptr || quantity == 0 || quantity > order->remaining)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			emit_transaction(message, source_sequence_, UftTransactionType::Cancel,
				order->side, order->price_ticks, quantity, order_id, engine);
			emit_book(message, UftBookAction::Reduce, order_id, 0, 0,
				order->price_ticks, quantity, order->side, false, engine);
			order->remaining -= quantity;
			if (order->remaining == 0)
				orders_.erase(order_id);
		}

		template<typename Engine>
		void cancel_all(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto order_id = read_u64(message + 11);
			const auto* order = orders_.find(order_id);
			if (order == nullptr)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const auto snapshot = *order;
			emit_transaction(message, source_sequence_, UftTransactionType::Cancel,
				snapshot.side, snapshot.price_ticks, snapshot.remaining, order_id,
				engine);
			emit_book(message, UftBookAction::Delete, order_id, 0, 0,
				snapshot.price_ticks, snapshot.remaining, snapshot.side, false, engine);
			orders_.erase(order_id);
		}

		template<typename Engine>
		void replace(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto old_id = read_u64(message + 11);
			const auto new_id = read_u64(message + 19);
			const auto quantity = read_u32(message + 27);
			const auto price = read_u32(message + 31);
			const auto* found = orders_.find(old_id);
			if (found == nullptr || new_id == 0 || quantity == 0 || price == 0 ||
				orders_.find(new_id) != nullptr)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const auto old_order = *found;
			orders_.erase(old_id);
			const Order replacement{static_cast<PriceTicks>(price), quantity,
				old_order.side};
			const auto inserted = orders_.insert(new_id, replacement);
			if (inserted != FixedHashInsertResult::Inserted)
			{
				count_insert_failure(inserted);
				return;
			}
			emit_transaction(message, source_sequence_, UftTransactionType::Cancel,
				old_order.side, old_order.price_ticks, old_order.remaining, old_id,
				engine);
			emit_order(message, new_id, replacement, engine);
			emit_book(message, UftBookAction::Replace, old_id, new_id, 0,
				replacement.price_ticks, quantity, replacement.side, false, engine);
		}

		template<typename Engine>
		void non_cross_trade(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto match_id = read_u64(message + 36);
			const Match match{static_cast<PriceTicks>(read_u32(message + 32)),
				read_u32(message + 20), read_u64(message + 11),
				side_of(message[19]), true};
			if (match.quantity == 0 || match.price_ticks <= 0 || match.side == UftSide::Unknown)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const auto first = matches_.insert(match_id, match);
			if (first == FixedHashInsertResult::Inserted)
				emit_book(message, UftBookAction::Trade, match.order_id, 0, match_id,
					match.price_ticks, match.quantity, match.side, true, engine);
			if (first == FixedHashInsertResult::Inserted)
				emit_transaction(message, match_id, UftTransactionType::Trade,
					match.side, match.price_ticks, match.quantity, match.order_id, engine);
			else if (first == FixedHashInsertResult::Duplicate)
				++stats_.duplicate_matches_suppressed;
			else
				count_insert_failure(first);
		}

		template<typename Engine>
		void cross_trade(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto match_id = read_u64(message + 31);
			const Match match{static_cast<PriceTicks>(read_u32(message + 27)),
				read_u64(message + 11), 0, UftSide::Unknown, true};
			if (match.quantity == 0 || match.price_ticks <= 0)
			{
				++stats_.malformed_or_missing_orders;
				return;
			}
			const auto first = matches_.insert(match_id, match);
			if (first == FixedHashInsertResult::Inserted)
				emit_book(message, UftBookAction::Trade, match.order_id, 0, match_id,
					match.price_ticks, match.quantity, match.side, true, engine);
			if (first == FixedHashInsertResult::Inserted)
				emit_transaction(message, match_id, UftTransactionType::Trade,
					match.side, match.price_ticks, match.quantity, match.order_id, engine);
			else if (first == FixedHashInsertResult::Duplicate)
				++stats_.duplicate_matches_suppressed;
			else
				count_insert_failure(first);
		}

		template<typename Engine>
		void broken_trade(const std::uint8_t* message, Engine& engine) noexcept
		{
			const auto match_id = read_u64(message + 11);
			const auto* found = matches_.find(match_id);
			if (found == nullptr)
			{
				++stats_.unknown_broken_trades;
				return;
			}
			const auto match = *found;
			emit_book(message, UftBookAction::TradeBust, match.order_id, 0, match_id,
				match.price_ticks, match.quantity, match.side, match.emitted, engine);
			if (match.emitted)
				emit_transaction(message, match_id, UftTransactionType::TradeBust,
					match.side, match.price_ticks, match.quantity, match.order_id,
					engine);
			else
				++stats_.broken_trades_without_output;
			matches_.erase(match_id);
		}

		InstrumentId instrument_id_{kInvalidInstrumentId};
		std::uint16_t locator_{0};
		std::uint64_t source_sequence_{0};
		FixedHashTable<Order> orders_;
		FixedHashTable<Match> matches_;
		UftAdapterStats stats_{};
	};
}
