#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <thread>

namespace hftbench
{
	enum class FullPolicy { Reject, Spin, DropNewest };
	enum class PushResult { Pushed, Full, Dropped };

	template<typename T, std::size_t Capacity>
	class SpscRing
	{
		static_assert(Capacity >= 2, "SPSC capacity must be at least two");
		static_assert((Capacity & (Capacity - 1)) == 0, "SPSC capacity must be a power of two");

	public:
		bool try_push(const T& value) noexcept(std::is_nothrow_copy_assignable<T>::value)
		{
			const std::size_t head = _producer.head;
			const std::size_t next = (head + 1) & (Capacity - 1);
			if (next == _consumer.tail.load(std::memory_order_acquire))
				return false;

			_buffer[head] = value;
			_producer.head = next;
			_producer.published.store(next, std::memory_order_release);
			return true;
		}

		bool try_pop(T& value) noexcept(std::is_nothrow_copy_assignable<T>::value)
		{
			const std::size_t tail = _consumer.local_tail;
			if (tail == _producer.published.load(std::memory_order_acquire))
				return false;

			value = _buffer[tail];
			const std::size_t next = (tail + 1) & (Capacity - 1);
			_consumer.local_tail = next;
			_consumer.tail.store(next, std::memory_order_release);
			return true;
		}

		PushResult push(const T& value, FullPolicy policy, uint64_t* dropped = nullptr)
		{
			if (policy == FullPolicy::Spin)
			{
				while (!try_push(value))
					std::this_thread::yield();
				return PushResult::Pushed;
			}
			if (try_push(value))
				return PushResult::Pushed;
			if (policy == FullPolicy::DropNewest)
			{
				if (dropped != nullptr)
					++*dropped;
				return PushResult::Dropped;
			}
			return PushResult::Full;
		}

	private:
		struct alignas(64) ProducerState
		{
			std::atomic<std::size_t> published{0};
			std::size_t head{0};
		};

		struct alignas(64) ConsumerState
		{
			std::atomic<std::size_t> tail{0};
			std::size_t local_tail{0};
		};

		std::array<T, Capacity> _buffer{};
		ProducerState _producer;
		ConsumerState _consumer;
	};
}
