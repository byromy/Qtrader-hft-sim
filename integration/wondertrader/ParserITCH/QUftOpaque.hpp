#pragma once

#include "qtrader/UftTypes.hpp"

#include <cstddef>
#include <cstdint>

struct QUftOpaque;

using QUftOrderCallback = void (*)(void*,
	const qtrader::UftOrderDetail*) noexcept;
using QUftTransactionCallback = void (*)(void*,
	const qtrader::UftTransaction*) noexcept;

struct QUftOpaqueStats
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
	std::uint64_t errors{0};
	std::uint64_t capacity_exhaustions{0};
	std::uint64_t peak_active_orders{0};
	std::uint64_t order_events_delivered{0};
	std::uint64_t transaction_events_delivered{0};
	std::uint64_t callback_deliveries{0};
	std::uint64_t undelivered_events{0};
};

QUftOpaque* quft_create(std::uint16_t locator, std::uint32_t instrument_id,
	std::uint32_t strategy_id, const char* standard_code,
	std::size_t maximum_active_orders, std::size_t maximum_unique_matches,
	void* strategy, QUftOrderCallback order_callback,
	QUftTransactionCallback transaction_callback) noexcept;
void quft_destroy(QUftOpaque* runner) noexcept;
void quft_message(QUftOpaque* runner, const std::uint8_t* message,
	std::size_t size) noexcept;
QUftOpaqueStats quft_stats(const QUftOpaque* runner) noexcept;
std::size_t quft_active_orders(const QUftOpaque* runner) noexcept;
std::size_t quft_startup_table_memory(const QUftOpaque* runner) noexcept;
std::size_t quft_runner_static_memory() noexcept;

std::size_t quft_instrument_capacity() noexcept;
std::size_t quft_strategy_capacity() noexcept;
