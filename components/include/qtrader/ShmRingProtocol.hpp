#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "third_party/wondertrader/WTSStruct.h"

namespace wtshm
{
	constexpr uint64_t kMagic = 0x575453484D524E47ULL; // WTSHMRNG
	constexpr uint32_t kVersion = 1;
	constexpr uint64_t kNoSequence = std::numeric_limits<uint64_t>::max();
	constexpr std::size_t kCapacity = 8 * 1024;
	static_assert(std::atomic<uint64_t>::is_always_lock_free,
		"sequence ring requires lock-free 64-bit atomics");

	enum class DataType : uint32_t
	{
		Tick = 0,
		OrderQueue = 1,
		OrderDetail = 2,
		Transaction = 3
	};

	struct DataItem
	{
		uint32_t type;
		uint32_t reserved;
		union
		{
			wtp::WTSTickStruct tick;
			wtp::WTSOrdQueStruct queue;
			wtp::WTSOrdDtlStruct order;
			wtp::WTSTransStruct trans;
		};

		DataItem() { std::memset(this, 0, sizeof(*this)); }
	};

	static_assert(std::is_trivially_copyable<DataItem>::value,
		"shared-memory data items must be trivially copyable");
	static_assert(offsetof(DataItem, tick) == 8, "legacy shared-memory payload offset changed");

	struct LegacyQueue
	{
		uint64_t capacity;
		volatile uint64_t readable;
		volatile uint64_t writable;
		uint32_t pid;
		DataItem items[kCapacity];

		LegacyQueue()
			: capacity(kCapacity), readable(kNoSequence), writable(0), pid(0)
		{
		}
	};

	struct alignas(64) SequenceHeader
	{
		uint64_t magic;
		uint32_t version;
		uint32_t header_size;
		uint64_t capacity;
		uint64_t item_size;
		std::atomic<uint64_t> epoch;
		std::atomic<uint64_t> published;
		uint32_t pid;
		uint32_t reserved;

		SequenceHeader()
			: magic(kMagic), version(kVersion), header_size(sizeof(SequenceHeader)),
			  capacity(kCapacity), item_size(sizeof(DataItem)),
			  epoch(0), published(kNoSequence), pid(0), reserved(0)
		{
		}
	};

	struct alignas(64) SequenceSlot
	{
		std::atomic<uint64_t> sequence;
		DataItem item;

		SequenceSlot() : sequence(kNoSequence), item() {}
	};

	struct SequenceQueue
	{
		SequenceHeader header;
		SequenceSlot slots[kCapacity];
	};

	inline uint64_t make_epoch(uint32_t pid)
	{
		const uint64_t ticks = static_cast<uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		return ticks ^ (static_cast<uint64_t>(pid) << 32);
	}

	inline bool valid(const SequenceQueue& queue)
	{
		return queue.header.magic == kMagic &&
			queue.header.version == kVersion &&
			queue.header.header_size == sizeof(SequenceHeader) &&
			queue.header.capacity == kCapacity &&
			queue.header.item_size == sizeof(DataItem);
	}

	inline void publish(SequenceQueue& queue, uint64_t sequence, const DataItem& item)
	{
		SequenceSlot& slot = queue.slots[sequence & (kCapacity - 1)];
		std::memcpy(&slot.item, &item, sizeof(item));
		slot.sequence.store(sequence, std::memory_order_release);
		queue.header.published.store(sequence, std::memory_order_release);
	}

	class SequenceReader
	{
	public:
		SequenceReader() : _next(0), _epoch(0), _dropped(0), _initialized(false) {}

		void attach(const SequenceQueue& queue, bool start_at_latest)
		{
			_epoch = queue.header.epoch.load(std::memory_order_acquire);
			const uint64_t published = queue.header.published.load(std::memory_order_acquire);
			_next = published == kNoSequence ? 0 : (start_at_latest ? published + 1 : oldest(published));
			_initialized = true;
		}

		bool restarted(const SequenceQueue& queue) const
		{
			return _initialized && queue.header.epoch.load(std::memory_order_acquire) != _epoch;
		}

		bool try_read(const SequenceQueue& queue, DataItem& output)
		{
			if (!_initialized)
				attach(queue, true);

			const uint64_t published = queue.header.published.load(std::memory_order_acquire);
			if (published == kNoSequence || _next > published)
				return false;

			const uint64_t oldest_available = oldest(published);
			if (_next < oldest_available)
			{
				_dropped += oldest_available - _next;
				_next = oldest_available;
			}

			const SequenceSlot& slot = queue.slots[_next & (kCapacity - 1)];
			const uint64_t before = slot.sequence.load(std::memory_order_acquire);
			if (before != _next)
			{
				if (before > _next && before != kNoSequence)
				{
					_dropped += before - _next;
					_next = before;
				}
				return false;
			}

			std::memcpy(&output, &slot.item, sizeof(output));
			const uint64_t after = slot.sequence.load(std::memory_order_acquire);
			if (after != before)
			{
				_dropped += after > _next ? after - _next : 1;
				_next = after == kNoSequence ? _next + 1 : after;
				return false;
			}

			++_next;
			return true;
		}

		uint64_t next_sequence() const { return _next; }
		uint64_t dropped() const { return _dropped; }
		uint64_t epoch() const { return _epoch; }

	private:
		static uint64_t oldest(uint64_t published)
		{
			return published >= kCapacity - 1 ? published - (kCapacity - 1) : 0;
		}

		uint64_t _next;
		uint64_t _epoch;
		uint64_t _dropped;
		bool _initialized;
	};
}
