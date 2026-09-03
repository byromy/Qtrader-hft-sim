#pragma once

#ifdef QTRADER_EXTERNAL_WONDERTRADER
#include "WTSStruct.h"
#else
#include "third_party/wondertrader/WTSStruct.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace qtrader
{
	class WtsEventFingerprint
	{
	public:
		void reset() noexcept
		{
			state_ = 0x6a09e667f3bcc909ULL;
			count_ = 0;
		}

		void add(const wtp::WTSOrdDtlStruct& value) noexcept
		{
			mix(0x4f524445525f4454ULL); // ORDER_DT
			mix_bytes(value.exchg, sizeof(value.exchg));
			mix_bytes(value.code, sizeof(value.code));
			mix(value.trading_date);
			mix(value.action_date);
			mix(value.action_time);
			mix(value.index);
			mix(double_bits(value.price));
			mix(value.volume);
			mix(static_cast<std::uint64_t>(value.side));
			mix(static_cast<std::uint64_t>(value.otype));
			mix(++count_);
		}

		void add(const wtp::WTSTransStruct& value) noexcept
		{
			mix(0x5452414e535f4454ULL); // TRANS_DT
			mix_bytes(value.exchg, sizeof(value.exchg));
			mix_bytes(value.code, sizeof(value.code));
			mix(value.trading_date);
			mix(value.action_date);
			mix(value.action_time);
			mix(static_cast<std::uint64_t>(value.index));
			mix(static_cast<std::uint64_t>(value.ttype));
			mix(static_cast<std::uint64_t>(value.side));
			mix(double_bits(value.price));
			mix(value.volume);
			mix(static_cast<std::uint64_t>(value.askorder));
			mix(static_cast<std::uint64_t>(value.bidorder));
			mix(++count_);
		}

		std::uint64_t digest() const noexcept { return avalanche(state_); }
		std::uint64_t count() const noexcept { return count_; }

	private:
		static std::uint64_t rotate_left(std::uint64_t value,
			unsigned amount) noexcept
		{
			return (value << amount) | (value >> (64U - amount));
		}

		static std::uint64_t avalanche(std::uint64_t value) noexcept
		{
			value ^= value >> 30U;
			value *= 0xbf58476d1ce4e5b9ULL;
			value ^= value >> 27U;
			value *= 0x94d049bb133111ebULL;
			return value ^ (value >> 31U);
		}

		void mix(std::uint64_t value) noexcept
		{
			state_ ^= avalanche(value + 0x9e3779b97f4a7c15ULL + count_);
			state_ = rotate_left(state_, 27U) * 0x3c79ac492ba7b653ULL +
				0x1c69b3f74ac4ae35ULL;
		}

		void mix_bytes(const char* bytes, std::size_t size) noexcept
		{
			std::uint64_t word = 0;
			unsigned shift = 0;
			for (std::size_t i = 0; i < size; ++i)
			{
				word |= static_cast<std::uint64_t>(
					static_cast<unsigned char>(bytes[i])) << shift;
				shift += 8U;
				if (shift == 64U)
				{
					mix(word);
					word = 0;
					shift = 0;
				}
			}
			if (shift != 0)
				mix(word ^ (static_cast<std::uint64_t>(size) << 56U));
		}

		static std::uint64_t double_bits(double value) noexcept
		{
			std::uint64_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		std::uint64_t state_{0x6a09e667f3bcc909ULL};
		std::uint64_t count_{0};
	};
}
