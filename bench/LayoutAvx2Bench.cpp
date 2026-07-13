#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	struct TickBookAoS
	{
		uint64_t sequence;
		uint64_t exchange_time;
		double bid_price[5];
		double ask_price[5];
		uint32_t bid_qty[5];
		uint32_t ask_qty[5];
		double last_price;
		double turnover;
	};

	struct TopOfBookSoA
	{
		std::vector<double> bid;
		std::vector<double> ask;
	};

#if defined(__GNUC__)
	__attribute__((optimize("no-tree-vectorize")))
#endif
	double sum_spread_aos(const TickBookAoS* books, std::size_t count)
	{
		double sum = 0;
		for (std::size_t i = 0; i < count; ++i)
			sum += books[i].ask_price[0] - books[i].bid_price[0];
		return sum;
	}

#if defined(__GNUC__)
	__attribute__((optimize("no-tree-vectorize")))
#endif
	double sum_spread_soa_scalar(const TopOfBookSoA& books)
	{
		double sum = 0;
		for (std::size_t i = 0; i < books.bid.size(); ++i)
			sum += books.ask[i] - books.bid[i];
		return sum;
	}

	double sum_spread_soa_avx2(const TopOfBookSoA& books)
	{
		__m256d sum = _mm256_setzero_pd();
		std::size_t i = 0;
		for (; i + 4 <= books.bid.size(); i += 4)
		{
			const __m256d bid = _mm256_loadu_pd(books.bid.data() + i);
			const __m256d ask = _mm256_loadu_pd(books.ask.data() + i);
			sum = _mm256_add_pd(sum, _mm256_sub_pd(ask, bid));
		}
		alignas(32) double lanes[4];
		_mm256_store_pd(lanes, sum);
		double result = lanes[0] + lanes[1] + lanes[2] + lanes[3];
		for (; i < books.bid.size(); ++i)
			result += books.ask[i] - books.bid[i];
		return result;
	}

	template<typename Function>
	void run(const char* name, std::size_t count, uint32_t rounds, Function function)
	{
		volatile double checksum = function();
		const auto begin = Clock::now();
		for (uint32_t round = 0; round < rounds; ++round)
			checksum = checksum + function();
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
		const double items = static_cast<double>(count) * rounds;
		std::printf("%-16s ns/tick=%7.3f throughput=%12.0f tick/s checksum=%.3f\n",
			name, elapsed / items, items * 1e9 / elapsed, checksum);
	}
}

int main(int argc, char** argv)
{
	const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1U << 20;
	const uint32_t rounds = argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 20;
	std::vector<TickBookAoS> aos(count);
	TopOfBookSoA soa{std::vector<double>(count), std::vector<double>(count)};

	for (std::size_t i = 0; i < count; ++i)
	{
		const double bid = 3500.0 + static_cast<double>(i & 255) * 0.2;
		const double ask = bid + 0.2;
		aos[i].bid_price[0] = bid;
		aos[i].ask_price[0] = ask;
		soa.bid[i] = bid;
		soa.ask[i] = ask;
	}

	std::printf("ticks=%zu rounds=%u aos_bytes=%zu soa_hot_bytes=%zu\n",
		count, rounds, sizeof(TickBookAoS) * count, sizeof(double) * count * 2);
	run("aos_scalar", count, rounds, [&]() { return sum_spread_aos(aos.data(), aos.size()); });
	run("soa_scalar", count, rounds, [&]() { return sum_spread_soa_scalar(soa); });
	run("soa_avx2", count, rounds, [&]() { return sum_spread_soa_avx2(soa); });
	return 0;
}
