#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace qtrader
{
	using InstrumentId = std::uint32_t;
	using StrategyId = std::uint32_t;
	using PriceTicks = std::int64_t;
	using Quantity = std::uint64_t;

	inline constexpr InstrumentId kInvalidInstrumentId =
		std::numeric_limits<InstrumentId>::max();
	inline constexpr StrategyId kInvalidStrategyId =
		std::numeric_limits<StrategyId>::max();

	enum class UftSide : std::uint8_t
	{
		Unknown = 0,
		Buy = 1,
		Sell = 2
	};

	enum class UftOrderType : std::uint8_t
	{
		Unknown = 0,
		Limit = 1,
		Market = 2
	};

	enum class UftTransactionType : std::uint8_t
	{
		Unknown = 0,
		Trade = 1,
		Cancel = 2,
		TradeBust = 3
	};

	struct UftOrderDetail
	{
		std::uint64_t source_sequence{0};
		std::uint64_t exchange_time_ns{0};
		std::uint64_t order_id{0};
		PriceTicks price_ticks{0};
		Quantity quantity{0};
		InstrumentId instrument_id{kInvalidInstrumentId};
		UftSide side{UftSide::Unknown};
		UftOrderType order_type{UftOrderType::Unknown};
		std::uint16_t reserved{0};
	};

	struct UftTransaction
	{
		std::uint64_t source_sequence{0};
		std::uint64_t exchange_time_ns{0};
		std::uint64_t transaction_id{0};
		std::uint64_t ask_order_id{0};
		std::uint64_t bid_order_id{0};
		PriceTicks price_ticks{0};
		Quantity quantity{0};
		InstrumentId instrument_id{kInvalidInstrumentId};
		UftSide side{UftSide::Unknown};
		UftTransactionType transaction_type{UftTransactionType::Unknown};
		std::uint16_t reserved{0};
	};

	static_assert(std::is_trivially_copyable<UftOrderDetail>::value,
		"UFT order details must remain trivially copyable");
	static_assert(std::is_trivially_copyable<UftTransaction>::value,
		"UFT transactions must remain trivially copyable");
	static_assert(sizeof(UftOrderDetail) == 48,
		"UFT order detail hot layout changed");
	static_assert(sizeof(UftTransaction) == 64,
		"UFT transaction hot layout changed");
}
