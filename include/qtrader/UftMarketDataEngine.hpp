#pragma once

#include "qtrader/UftTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace qtrader
{
	class UftStrategyContext;

	struct UftInstrumentMetadata
	{
		static constexpr std::size_t kMaximumStandardCode = 31;

		InstrumentId instrument_id{kInvalidInstrumentId};
		PriceTicks minimum_price_ticks{0};
		PriceTicks maximum_price_ticks{0};
		PriceTicks tick_size{0};
		std::array<char, kMaximumStandardCode + 1> standard_code{};
	};

	struct UftStrategyCallbacks
	{
		using Init = void (*)(void*, UftStrategyContext&) noexcept;
		using Session = void (*)(void*, const UftStrategyContext&,
			std::uint32_t) noexcept;
		using OrderDetail = void (*)(void*, const UftStrategyContext&,
			const UftOrderDetail&) noexcept;
		using Transaction = void (*)(void*, const UftStrategyContext&,
			const UftTransaction&) noexcept;

		Init on_init{nullptr};
		Session on_session_begin{nullptr};
		Session on_session_end{nullptr};
		OrderDetail on_order_detail{nullptr};
		Transaction on_transaction{nullptr};
	};

	class UftStrategyContext
	{
	public:
		StrategyId id() const noexcept { return strategy_id_; }

		bool stra_sub_order_details(InstrumentId instrument_id) noexcept
		{
			return subscribe(instrument_id, Channel::OrderDetail);
		}

		bool stra_sub_transactions(InstrumentId instrument_id) noexcept
		{
			return subscribe(instrument_id, Channel::Transaction);
		}

		const UftInstrumentMetadata* instrument(
			InstrumentId instrument_id) const noexcept
		{
			return lookup_ == nullptr ? nullptr : lookup_(engine_, instrument_id);
		}

		bool configuration_ok() const noexcept { return configuration_ok_; }

	private:
		enum class Channel : std::uint8_t
		{
			OrderDetail,
			Transaction
		};

		using Subscribe = bool (*)(void*, StrategyId, InstrumentId,
			Channel) noexcept;
		using Lookup = const UftInstrumentMetadata* (*)(const void*,
			InstrumentId) noexcept;

		bool subscribe(InstrumentId instrument_id, Channel channel) noexcept
		{
			const bool result = subscribe_ != nullptr &&
				subscribe_(engine_, strategy_id_, instrument_id, channel);
			configuration_ok_ = configuration_ok_ && result;
			return result;
		}

		template<std::size_t, std::size_t, std::size_t>
		friend class UftMarketDataEngine;

		void* engine_{nullptr};
		StrategyId strategy_id_{kInvalidStrategyId};
		Subscribe subscribe_{nullptr};
		Lookup lookup_{nullptr};
		bool configuration_ok_{true};
	};

	template<std::size_t InstrumentCapacity, std::size_t StrategyCapacity,
		std::size_t MaxStrategiesPerInstrument = 8>
	class UftMarketDataEngine
	{
		static_assert(InstrumentCapacity != 0, "instrument capacity is required");
		static_assert(StrategyCapacity != 0, "strategy capacity is required");
		static_assert(MaxStrategiesPerInstrument != 0,
			"subscriber capacity is required");
		static_assert(MaxStrategiesPerInstrument <= 65535,
			"subscriber count exceeds its compact representation");

		struct StrategySlot;

		struct alignas(64) InstrumentSlot
		{
			UftInstrumentMetadata metadata{};
			std::array<StrategySlot*, MaxStrategiesPerInstrument> order_details{};
			std::array<StrategySlot*, MaxStrategiesPerInstrument> transactions{};
			std::uint16_t order_detail_count{0};
			std::uint16_t transaction_count{0};
			bool registered{false};
		};

		struct alignas(64) StrategySlot
		{
			void* strategy{nullptr};
			UftStrategyCallbacks callbacks{};
			UftStrategyContext context{};
			bool registered{false};
		};

	public:
		bool register_instrument(InstrumentId instrument_id,
			const char* standard_code, PriceTicks minimum_price_ticks,
			PriceTicks maximum_price_ticks, PriceTicks tick_size) noexcept
		{
			if (configuration_frozen_ || instrument_id >= InstrumentCapacity ||
				standard_code == nullptr || tick_size <= 0 ||
				minimum_price_ticks > maximum_price_ticks)
				return false;
			auto& slot = instruments_[instrument_id];
			if (slot.registered)
				return false;
			std::size_t length = 0;
			while (length <= UftInstrumentMetadata::kMaximumStandardCode &&
				standard_code[length] != '\0')
				++length;
			if (length == 0 ||
				length > UftInstrumentMetadata::kMaximumStandardCode)
				return false;
			slot.metadata.instrument_id = instrument_id;
			slot.metadata.minimum_price_ticks = minimum_price_ticks;
			slot.metadata.maximum_price_ticks = maximum_price_ticks;
			slot.metadata.tick_size = tick_size;
			std::memcpy(slot.metadata.standard_code.data(), standard_code, length);
			slot.metadata.standard_code[length] = '\0';
			slot.registered = true;
		return true;
		}

		bool register_strategy(StrategyId strategy_id, void* strategy,
			UftStrategyCallbacks callbacks) noexcept
		{
			if (configuration_frozen_ || strategy_id >= StrategyCapacity ||
				strategy == nullptr)
				return false;
			auto& slot = strategies_[strategy_id];
			if (slot.registered)
				return false;
			slot.strategy = strategy;
			slot.callbacks = callbacks;
			slot.context.engine_ = this;
			slot.context.strategy_id_ = strategy_id;
			slot.context.subscribe_ = &subscribe_bridge;
			slot.context.lookup_ = &lookup_bridge;
			slot.context.configuration_ok_ = true;
			slot.registered = true;
		return true;
		}

		bool start() noexcept
		{
			if (configuration_frozen_)
				return false;
			configuration_frozen_ = true;
			for (auto& strategy : strategies_)
			{
				if (strategy.registered && strategy.callbacks.on_init != nullptr)
					strategy.callbacks.on_init(strategy.strategy, strategy.context);
				if (strategy.registered && !strategy.context.configuration_ok())
					return false;
			}
			running_ = true;
			return true;
		}

		void stop() noexcept { running_ = false; }
		bool running() const noexcept { return running_; }
		bool configuration_frozen() const noexcept
		{
			return configuration_frozen_;
		}

		void on_session_begin(std::uint32_t trading_date) noexcept
		{
			if (!running_)
				return;
			for (auto& slot : strategies_)
				if (slot.registered && slot.callbacks.on_session_begin != nullptr)
					slot.callbacks.on_session_begin(
						slot.strategy, slot.context, trading_date);
		}

		void on_session_end(std::uint32_t trading_date) noexcept
		{
			if (!running_)
				return;
			for (auto& slot : strategies_)
				if (slot.registered && slot.callbacks.on_session_end != nullptr)
					slot.callbacks.on_session_end(
						slot.strategy, slot.context, trading_date);
		}

		std::size_t publish_order_detail(
			const UftOrderDetail& event) const noexcept
		{
			if (!running_ || event.instrument_id >= InstrumentCapacity)
				return 0;
			const auto& instrument = instruments_[event.instrument_id];
			if (!instrument.registered)
				return 0;
			for (std::size_t index = 0;
				index < instrument.order_detail_count; ++index)
			{
				auto* strategy = instrument.order_details[index];
				strategy->callbacks.on_order_detail(
					strategy->strategy, strategy->context, event);
			}
			return instrument.order_detail_count;
		}

		std::size_t publish_transaction(
			const UftTransaction& event) const noexcept
		{
			if (!running_ || event.instrument_id >= InstrumentCapacity)
				return 0;
			const auto& instrument = instruments_[event.instrument_id];
			if (!instrument.registered)
				return 0;
			for (std::size_t index = 0;
				index < instrument.transaction_count; ++index)
			{
				auto* strategy = instrument.transactions[index];
				strategy->callbacks.on_transaction(
					strategy->strategy, strategy->context, event);
			}
			return instrument.transaction_count;
		}

		const UftInstrumentMetadata* instrument(
			InstrumentId instrument_id) const noexcept
		{
			if (instrument_id >= InstrumentCapacity ||
				!instruments_[instrument_id].registered)
				return nullptr;
			return &instruments_[instrument_id].metadata;
		}

	private:
		static bool subscribe_bridge(void* engine, StrategyId strategy_id,
			InstrumentId instrument_id,
			UftStrategyContext::Channel channel) noexcept
		{
			return static_cast<UftMarketDataEngine*>(engine)->subscribe(
				strategy_id, instrument_id, channel);
		}

		static const UftInstrumentMetadata* lookup_bridge(const void* engine,
			InstrumentId instrument_id) noexcept
		{
			return static_cast<const UftMarketDataEngine*>(engine)->instrument(
				instrument_id);
		}

		bool subscribe(StrategyId strategy_id, InstrumentId instrument_id,
			UftStrategyContext::Channel channel) noexcept
		{
			if (!configuration_frozen_ || running_ ||
				strategy_id >= StrategyCapacity ||
				instrument_id >= InstrumentCapacity)
				return false;
			auto& strategy = strategies_[strategy_id];
			auto& instrument = instruments_[instrument_id];
			if (!strategy.registered || !instrument.registered)
				return false;
			if (channel == UftStrategyContext::Channel::OrderDetail)
				return append_subscription(instrument.order_details,
					instrument.order_detail_count, &strategy,
					strategy.callbacks.on_order_detail != nullptr);
			return append_subscription(instrument.transactions,
				instrument.transaction_count, &strategy,
				strategy.callbacks.on_transaction != nullptr);
		}

		static bool append_subscription(
			std::array<StrategySlot*, MaxStrategiesPerInstrument>& subscribers,
			std::uint16_t& count, StrategySlot* strategy,
			bool callback_present) noexcept
		{
			if (!callback_present || count == MaxStrategiesPerInstrument)
				return false;
			for (std::size_t index = 0; index < count; ++index)
				if (subscribers[index] == strategy)
					return false;
			subscribers[count++] = strategy;
			return true;
		}

		std::array<InstrumentSlot, InstrumentCapacity> instruments_{};
		std::array<StrategySlot, StrategyCapacity> strategies_{};
		bool configuration_frozen_{false};
		bool running_{false};
	};
}
