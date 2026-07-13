#include "../WtHftComponents/DensePriceOrderBook.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;
	uint64_t now_ns()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now().time_since_epoch()).count());
	}

	struct NullSink
	{
		uint64_t checksum = 0;
		void on_trade(const wthft::Trade& trade) { checksum += trade.maker_id + trade.quantity; }
		void on_order(const wthft::OrderUpdate& update) { checksum += update.id + update.remaining; }
	};

	uint64_t percentile(const std::vector<uint64_t>& sorted, double value)
	{
		const std::size_t index = static_cast<std::size_t>(std::ceil(value * sorted.size()) - 1.0);
		return sorted[std::min(index, sorted.size() - 1)];
	}

	template<typename Book>
	void benchmark(const char* name, Book& book, uint32_t count)
	{
		NullSink sink;
		std::vector<uint64_t> submit_latency(count);
		const uint64_t submit_begin = now_ns();
		for (uint32_t i = 0; i < count; ++i)
		{
			const wthft::OrderRequest request{static_cast<uint64_t>(i + 1), wthft::Side::Buy,
				wthft::OrderType::Limit, 1000 - static_cast<int64_t>(i & 31), 10};
			const uint64_t begin = now_ns();
			book.submit(request, sink);
			submit_latency[i] = now_ns() - begin;
		}
		const uint64_t submit_elapsed = now_ns() - submit_begin;

		std::vector<uint64_t> cancel_latency(count);
		const uint64_t cancel_begin = now_ns();
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint64_t begin = now_ns();
			book.cancel(i + 1, sink);
			cancel_latency[i] = now_ns() - begin;
		}
		const uint64_t cancel_elapsed = now_ns() - cancel_begin;

		std::sort(submit_latency.begin(), submit_latency.end());
		std::sort(cancel_latency.begin(), cancel_latency.end());
		std::printf("%-9s submit=%9.0f op/s p50=%llu p99=%llu cancel=%9.0f op/s p50=%llu p99=%llu checksum=%llu\n",
			name, static_cast<double>(count) * 1e9 / submit_elapsed,
			static_cast<unsigned long long>(percentile(submit_latency, 0.50)),
			static_cast<unsigned long long>(percentile(submit_latency, 0.99)),
			static_cast<double>(count) * 1e9 / cancel_elapsed,
			static_cast<unsigned long long>(percentile(cancel_latency, 0.50)),
			static_cast<unsigned long long>(percentile(cancel_latency, 0.99)),
			static_cast<unsigned long long>(sink.checksum));
	}

	template<typename Book>
	void benchmark_match(const char* name, Book& book, uint32_t count, uint64_t id_base)
	{
		NullSink sink;
		for (uint32_t i = 0; i < count; ++i)
			book.submit({id_base + i, wthft::Side::Sell, wthft::OrderType::Limit,
				1001 + static_cast<int64_t>(i & 7), 1}, sink);
		std::vector<uint64_t> latency(count);
		const uint64_t total_begin = now_ns();
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint64_t begin = now_ns();
			book.submit({id_base + count + i, wthft::Side::Buy, wthft::OrderType::Market, 0, 1}, sink);
			latency[i] = now_ns() - begin;
		}
		const uint64_t elapsed = now_ns() - total_begin;
		std::sort(latency.begin(), latency.end());
		std::printf("%-9s match =%9.0f op/s p50=%llu p99=%llu checksum=%llu\n", name,
			static_cast<double>(count) * 1e9 / elapsed,
			static_cast<unsigned long long>(percentile(latency, 0.50)),
			static_cast<unsigned long long>(percentile(latency, 0.99)),
			static_cast<unsigned long long>(sink.checksum));
	}

	template<typename Book>
	void benchmark_modify(const char* name, Book& book, uint32_t count, uint64_t id_base)
	{
		NullSink sink;
		for (uint32_t i = 0; i < count; ++i)
			book.submit({id_base + i, wthft::Side::Buy, wthft::OrderType::Limit,
				1000 - static_cast<int64_t>(i & 31), 10}, sink);
		std::vector<uint64_t> latency(count);
		const uint64_t total_begin = now_ns();
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint64_t begin = now_ns();
			book.modify(id_base + i, 900 + static_cast<int64_t>(i & 31), 12, sink);
			latency[i] = now_ns() - begin;
		}
		const uint64_t elapsed = now_ns() - total_begin;
		std::sort(latency.begin(), latency.end());
		std::printf("%-9s modify=%9.0f op/s p50=%llu p99=%llu checksum=%llu\n", name,
			static_cast<double>(count) * 1e9 / elapsed,
			static_cast<unsigned long long>(percentile(latency, 0.50)),
			static_cast<unsigned long long>(percentile(latency, 0.99)),
			static_cast<unsigned long long>(sink.checksum));
	}
}

int main(int argc, char** argv)
{
	const uint32_t count = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 200000;
	wthft::PriceTimeOrderBook reference(count + 16);
	wthft::DensePriceOrderBook dense(count + 16, 1, 1, 4096);
	benchmark("reference", reference, count);
	benchmark("dense", dense, count);

	wthft::PriceTimeOrderBook reference_match(count + 16);
	wthft::DensePriceOrderBook dense_match(count + 16, 1, 1, 4096);
	benchmark_match("reference", reference_match, count, 1000000000ULL);
	benchmark_match("dense", dense_match, count, 2000000000ULL);

	wthft::PriceTimeOrderBook reference_modify(count + 16);
	wthft::DensePriceOrderBook dense_modify(count + 16, 1, 1, 4096);
	benchmark_modify("reference", reference_modify, count, 3000000000ULL);
	benchmark_modify("dense", dense_modify, count, 4000000000ULL);
	return 0;
}
