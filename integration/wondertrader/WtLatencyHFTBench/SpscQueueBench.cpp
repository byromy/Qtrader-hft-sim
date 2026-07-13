#include "SpscRing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	struct TickEvent
	{
		uint64_t sequence;
		uint64_t enqueue_ns;
		double bid;
		double ask;
		uint32_t bid_qty;
		uint32_t ask_qty;
	};

	uint64_t now_ns()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now().time_since_epoch()).count());
	}

	uint64_t percentile(const std::vector<uint64_t>& sorted, double p)
	{
		const std::size_t index = static_cast<std::size_t>(
			std::ceil(p * static_cast<double>(sorted.size())) - 1.0);
		return sorted[std::min(index, sorted.size() - 1)];
	}

	void report_latency(const char* name, std::vector<uint64_t>& samples)
	{
		std::sort(samples.begin(), samples.end());
		long double total = 0;
		for (uint64_t sample : samples)
			total += sample;
		const double mean = static_cast<double>(total / samples.size());
		std::printf("%-12s latency mean=%9.2f ns p50=%llu p99=%llu p99.9=%llu max=%llu\n",
			name, mean,
			static_cast<unsigned long long>(percentile(samples, 0.50)),
			static_cast<unsigned long long>(percentile(samples, 0.99)),
			static_cast<unsigned long long>(percentile(samples, 0.999)),
			static_cast<unsigned long long>(samples.back()));
	}

	template<typename Queue, typename Push, typename Pop>
	void run_throughput(const char* name, Queue& queue, Push push, Pop pop,
		uint32_t warmup, uint32_t measured)
	{
		const uint64_t total = static_cast<uint64_t>(warmup) + measured;
		std::atomic<bool> start{false};
		std::atomic<uint64_t> checksum{0};

		std::thread consumer([&]() {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			TickEvent event{};
			for (uint64_t received = 0; received < total;)
			{
				if (!pop(queue, event))
					continue;
				checksum.fetch_add(event.sequence, std::memory_order_relaxed);
				++received;
			}
		});

		start.store(true, std::memory_order_release);
		uint64_t begin = 0;
		for (uint64_t sent = 0; sent < total; ++sent)
		{
			if (sent == warmup)
				begin = now_ns();
			TickEvent event{sent, 0, 3500.0, 3500.2, 10, 12};
			while (!push(queue, event))
			{
			}
		}
		consumer.join();
		const uint64_t elapsed = now_ns() - begin;
		const double throughput = static_cast<double>(measured) * 1e9 / elapsed;
		std::printf("%-12s throughput=%10.0f msg/s\n", name, throughput);
		if (checksum.load(std::memory_order_relaxed) == 0)
			std::abort();
	}

	template<typename Queue, typename Push, typename Pop>
	void run_latency(const char* name, Queue& queue, Push push, Pop pop,
		uint32_t warmup, uint32_t measured)
	{
		const uint64_t total = static_cast<uint64_t>(warmup) + measured;
		std::vector<uint64_t> latency(measured);
		std::atomic<bool> start{false};
		std::atomic<uint64_t> consumed{0};

		std::thread consumer([&]() {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			TickEvent event{};
			for (uint64_t received = 0; received < total;)
			{
				if (!pop(queue, event))
					continue;
				if (received >= warmup)
					latency[received - warmup] = now_ns() - event.enqueue_ns;
				++received;
				consumed.store(received, std::memory_order_release);
			}
		});

		start.store(true, std::memory_order_release);
		for (uint64_t sent = 0; sent < total; ++sent)
		{
			TickEvent event{sent, now_ns(), 3500.0, 3500.2, 10, 12};
			while (!push(queue, event))
			{
			}
			while (consumed.load(std::memory_order_acquire) != sent + 1)
			{
			}
		}
		consumer.join();
		report_latency(name, latency);
	}

	struct MutexQueue
	{
		std::mutex mutex;
		std::deque<TickEvent> queue;
	};

	struct SpinQueue
	{
		struct Guard
		{
			explicit Guard(std::atomic_flag& flag) : _flag(flag)
			{
				while (_flag.test_and_set(std::memory_order_acquire))
					std::this_thread::yield();
			}
			~Guard() { _flag.clear(std::memory_order_release); }
			std::atomic_flag& _flag;
		};

		std::atomic_flag lock = ATOMIC_FLAG_INIT;
		std::deque<TickEvent> queue;
	};

	void run_direct(uint32_t warmup, uint32_t measured)
	{
		volatile uint64_t checksum = 0;
		const uint64_t total = static_cast<uint64_t>(warmup) + measured;
		uint64_t begin = 0;
		for (uint64_t i = 0; i < total; ++i)
		{
			if (i == warmup) begin = now_ns();
			TickEvent event{i, 0, 3500.0, 3500.2, 10, 12};
			checksum += event.sequence;
		}
		const uint64_t elapsed = now_ns() - begin;
		std::printf("%-12s throughput=%10.0f msg/s\n", "direct",
			static_cast<double>(measured) * 1e9 / elapsed);
		if (checksum == 0) std::abort();
	}

	void run_direct_latency(uint32_t warmup, uint32_t measured)
	{
		std::vector<uint64_t> latency(measured);
		volatile uint64_t checksum = 0;
		for (uint64_t i = 0; i < static_cast<uint64_t>(warmup) + measured; ++i)
		{
			TickEvent event{i, 0, 3500.0, 3500.2, 10, 12};
			const uint64_t begin = now_ns();
			checksum += event.sequence;
			const uint64_t end = now_ns();
			if (i >= warmup) latency[i - warmup] = end - begin;
		}
		report_latency("direct", latency);
		if (checksum == 0) std::abort();
	}
}

int main(int argc, char** argv)
{
	const uint32_t measured = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 1000000;
	const uint32_t warmup = argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 100000;
	run_direct(warmup, measured);

	hftbench::SpscRing<TickEvent, 65536> ring_throughput;
	run_throughput("spsc", ring_throughput,
		[](auto& q, const TickEvent& event) { return q.try_push(event); },
		[](auto& q, TickEvent& event) { return q.try_pop(event); }, warmup, measured);

	MutexQueue mutex_throughput;
	run_throughput("mutex", mutex_throughput,
		[](MutexQueue& q, const TickEvent& event) {
			std::lock_guard<std::mutex> lock(q.mutex);
			q.queue.push_back(event);
			return true;
		},
		[](MutexQueue& q, TickEvent& event) {
			std::lock_guard<std::mutex> lock(q.mutex);
			if (q.queue.empty())
				return false;
			event = q.queue.front();
			q.queue.pop_front();
			return true;
		}, warmup, measured);

	SpinQueue spin_throughput;
	run_throughput("spinlock", spin_throughput,
		[](SpinQueue& q, const TickEvent& event) {
			SpinQueue::Guard guard(q.lock);
			q.queue.push_back(event);
			return true;
		},
		[](SpinQueue& q, TickEvent& event) {
			SpinQueue::Guard guard(q.lock);
			if (q.queue.empty()) return false;
			event = q.queue.front();
			q.queue.pop_front();
			return true;
		}, warmup, measured);

	hftbench::SpscRing<TickEvent, 65536> ring_latency;
	run_latency("spsc", ring_latency,
		[](auto& q, const TickEvent& event) { return q.try_push(event); },
		[](auto& q, TickEvent& event) { return q.try_pop(event); }, warmup, measured);

	MutexQueue mutex_latency;
	run_latency("mutex", mutex_latency,
		[](MutexQueue& q, const TickEvent& event) {
			std::lock_guard<std::mutex> lock(q.mutex);
			q.queue.push_back(event);
			return true;
		},
		[](MutexQueue& q, TickEvent& event) {
			std::lock_guard<std::mutex> lock(q.mutex);
			if (q.queue.empty())
				return false;
			event = q.queue.front();
			q.queue.pop_front();
			return true;
		}, warmup, measured);

	SpinQueue spin_latency;
	run_latency("spinlock", spin_latency,
		[](SpinQueue& q, const TickEvent& event) {
			SpinQueue::Guard guard(q.lock);
			q.queue.push_back(event);
			return true;
		},
		[](SpinQueue& q, TickEvent& event) {
			SpinQueue::Guard guard(q.lock);
			if (q.queue.empty()) return false;
			event = q.queue.front();
			q.queue.pop_front();
			return true;
		}, warmup, measured);
	run_direct_latency(warmup, measured);

	hftbench::SpscRing<TickEvent, 2> policy_ring;
	TickEvent policy_event{};
	uint64_t dropped = 0;
	if (policy_ring.push(policy_event, hftbench::FullPolicy::Reject) != hftbench::PushResult::Pushed ||
		policy_ring.push(policy_event, hftbench::FullPolicy::Reject) != hftbench::PushResult::Full ||
		policy_ring.push(policy_event, hftbench::FullPolicy::DropNewest, &dropped) != hftbench::PushResult::Dropped ||
		dropped != 1)
		return 2;
	std::thread policy_consumer([&]() {
		TickEvent output{};
		while (!policy_ring.try_pop(output)) std::this_thread::yield();
	});
	const auto spin_result = policy_ring.push(policy_event, hftbench::FullPolicy::Spin);
	policy_consumer.join();
	if (spin_result != hftbench::PushResult::Pushed) return 3;
	std::printf("full_policy  reject=ok drop_newest=ok dropped=%llu spin=ok\n",
		static_cast<unsigned long long>(dropped));

	return 0;
}
