#include "qtrader/SpscRing.hpp"
#include "qtrader/ShmRingProtocol.hpp"
#include "qtrader/PriceTimeOrderBook.hpp"
#ifdef WT_DENSE_ORDERBOOK
#include "qtrader/DensePriceOrderBook.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifndef _MSC_VER
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
	using Clock = std::chrono::steady_clock;

	uint64_t now_ns()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now().time_since_epoch()).count());
	}

	struct BookEvent
	{
		uint64_t id;
		int64_t price;
		uint64_t quantity;
		uint64_t source_ns;
		uint64_t shm_ns;
		wthft::Side side;
		wthft::OrderType type;
	};

	struct StageSample
	{
		uint64_t shm;
		uint64_t spsc;
		uint64_t book_strategy;
		uint64_t end_to_end;
	};

	struct PipelineSink
	{
		uint64_t trades = 0;
		uint64_t updates = 0;
		uint64_t volume = 0;
		void on_trade(const wthft::Trade& trade) { ++trades; volume += trade.quantity; }
		void on_order(const wthft::OrderUpdate&) { ++updates; }
	};

	class Mapping
	{
	public:
		Mapping()
		{
#ifndef _MSC_VER
			_path = "/tmp/wt_level2_pipeline.shm";
			_fd = ::open(_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
			if (_fd < 0 || ::ftruncate(_fd, sizeof(wtshm::SequenceQueue)) != 0)
				std::abort();
			_address = ::mmap(nullptr, sizeof(wtshm::SequenceQueue), PROT_READ | PROT_WRITE,
				MAP_SHARED, _fd, 0);
			if (_address == MAP_FAILED) std::abort();
#else
			_address = ::operator new(sizeof(wtshm::SequenceQueue));
#endif
			_queue = new(_address) wtshm::SequenceQueue();
			_queue->header.epoch.store(wtshm::make_epoch(1), std::memory_order_release);
		}

		~Mapping()
		{
#ifndef _MSC_VER
			::munmap(_address, sizeof(wtshm::SequenceQueue));
			::close(_fd);
			::unlink(_path.c_str());
#else
			::operator delete(_address);
#endif
		}

		wtshm::SequenceQueue& queue() { return *_queue; }

	private:
		void* _address = nullptr;
		wtshm::SequenceQueue* _queue = nullptr;
#ifndef _MSC_VER
		int _fd = -1;
		std::string _path;
#endif
	};

	std::vector<wtp::WTSOrdDtlStruct> load_events(const char* path, std::size_t generated_count)
	{
		std::vector<wtp::WTSOrdDtlStruct> events;
		if (path != nullptr)
		{
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input) return events;
			const std::streamsize size = input.tellg();
			if (size <= 0 || size % sizeof(wtp::WTSOrdDtlStruct) != 0) return events;
			events.resize(static_cast<std::size_t>(size) / sizeof(wtp::WTSOrdDtlStruct));
			input.seekg(0);
			input.read(reinterpret_cast<char*>(events.data()), size);
			return events;
		}

		events.resize(generated_count);
		for (std::size_t i = 0; i < generated_count; ++i)
		{
			auto& event = events[i];
			event.index = i + 1;
			event.side = (i & 1) == 0 ? BDT_Buy : BDT_Sell;
			event.otype = ODT_LimitPrice;
			event.price = (i & 1) == 0 ? 99.0 - static_cast<double>(i & 7) :
				101.0 + static_cast<double>(i & 7);
			event.volume = 1 + static_cast<uint32_t>(i & 15);
		}
		return events;
	}

	struct PriceWindow
	{
		int64_t minimum = 1;
		int64_t tick_size = 1;
		uint32_t levels = 1;
	};

	PriceWindow infer_price_window(const std::vector<wtp::WTSOrdDtlStruct>& events)
	{
		int64_t minimum = std::numeric_limits<int64_t>::max();
		int64_t maximum = 0;
		int64_t first = 0;
		int64_t tick = 0;
		for (const auto& event : events)
		{
			if (event.price <= 0) continue;
			const int64_t price = static_cast<int64_t>(std::llround(event.price * 10000.0));
			minimum = std::min(minimum, price);
			maximum = std::max(maximum, price);
			if (first == 0) first = price;
			else tick = std::gcd(tick, std::llabs(price - first));
		}
		if (minimum == std::numeric_limits<int64_t>::max()) return {};
		if (tick == 0) tick = 1;
		const uint64_t levels = static_cast<uint64_t>((maximum - minimum) / tick) + 1;
		if (levels > std::numeric_limits<uint32_t>::max()) std::abort();
		return {minimum, tick, static_cast<uint32_t>(levels)};
	}

	uint64_t percentile(const std::vector<uint64_t>& sorted, double p)
	{
		const std::size_t index = static_cast<std::size_t>(std::ceil(p * sorted.size()) - 1.0);
		return sorted[std::min(index, sorted.size() - 1)];
	}

	template<typename Field>
	void report(const char* name, const std::vector<StageSample>& samples, Field field)
	{
		std::vector<uint64_t> values;
		values.reserve(samples.size());
		long double total = 0;
		for (const auto& sample : samples)
		{
			const uint64_t value = field(sample);
			values.push_back(value);
			total += value;
		}
		std::sort(values.begin(), values.end());
		std::printf("%-14s mean=%9.2f ns p50=%llu p99=%llu p99.9=%llu max=%llu\n",
			name, static_cast<double>(total / values.size()),
			static_cast<unsigned long long>(percentile(values, 0.50)),
			static_cast<unsigned long long>(percentile(values, 0.99)),
			static_cast<unsigned long long>(percentile(values, 0.999)),
			static_cast<unsigned long long>(values.back()));
	}
}

int main(int argc, char** argv)
{
	const char* input_path = argc > 1 && argv[1][0] != '-' ? argv[1] : nullptr;
	const std::size_t generated_count = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 200000;
	const bool paced = argc > 3 && std::string(argv[3]) == "paced";
	auto source = load_events(input_path, generated_count);
	if (source.empty())
	{
		std::fprintf(stderr, "no Level2 order-detail events loaded\n");
		return 1;
	}

	Mapping mapping;
	auto& shm = mapping.queue();
	hftbench::SpscRing<BookEvent, 65536> spsc;
	std::vector<uint64_t> source_time(source.size());
	std::vector<StageSample> samples(source.size());
	std::atomic<uint64_t> shm_consumed{0};
	std::atomic<uint64_t> completed{0};
	std::atomic<bool> start{false};
	std::atomic<bool> failed{false};

	PipelineSink sink;
	const std::size_t book_capacity = source.size() + source.size() / 1024 + 64;
#ifdef WT_DENSE_ORDERBOOK
	const PriceWindow window = infer_price_window(source);
	wthft::DensePriceOrderBook book(book_capacity, window.minimum, window.tick_size, window.levels);
#else
	wthft::PriceTimeOrderBook book(book_capacity);
#endif

	std::thread consumer([&]() {
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		BookEvent event{};
		for (std::size_t received = 0; received < source.size();)
		{
			if (!spsc.try_pop(event)) continue;
			const uint64_t book_begin = now_ns();
			if (!book.submit({event.id, event.side, event.type, event.price, event.quantity}, sink))
				failed.store(true, std::memory_order_relaxed);
			if ((received & 1023) == 1023 && book.best_ask() != 0)
			{
				const uint64_t strategy_id = (1ULL << 63) + received;
				book.submit({strategy_id, wthft::Side::Buy, wthft::OrderType::Market, 0, 1}, sink);
			}
			const uint64_t end = now_ns();
			samples[received] = {event.shm_ns - event.source_ns, book_begin - event.shm_ns,
				end - book_begin, end - event.source_ns};
			++received;
			completed.store(received, std::memory_order_release);
		}
	});

	std::thread shm_reader([&]() {
		wtshm::SequenceReader reader;
		reader.attach(shm, false);
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		wtshm::DataItem item;
		while (reader.next_sequence() < source.size())
		{
			if (!reader.try_read(shm, item)) continue;
			const uint64_t sequence = reader.next_sequence() - 1;
			const auto& order = item.order;
			BookEvent event{order.index,
				static_cast<int64_t>(std::llround(order.price * 10000.0)), order.volume,
				source_time[sequence], now_ns(),
				order.side == BDT_Buy ? wthft::Side::Buy : wthft::Side::Sell,
				order.otype == ODT_AnyPrice ? wthft::OrderType::Market : wthft::OrderType::Limit};
			while (!spsc.try_push(event)) std::this_thread::yield();
			shm_consumed.store(sequence + 1, std::memory_order_release);
		}
		if (reader.dropped() != 0) failed.store(true, std::memory_order_relaxed);
	});

	start.store(true, std::memory_order_release);
	const uint64_t begin = now_ns();
	for (uint64_t sequence = 0; sequence < source.size(); ++sequence)
	{
		while (sequence >= shm_consumed.load(std::memory_order_acquire) + wtshm::kCapacity - 1)
			std::this_thread::yield();
		wtshm::DataItem item;
		item.type = static_cast<uint32_t>(wtshm::DataType::OrderDetail);
		std::memcpy(&item.order, &source[sequence], sizeof(source[sequence]));
		source_time[sequence] = now_ns();
		wtshm::publish(shm, sequence, item);
		if (paced)
		{
			while (completed.load(std::memory_order_acquire) != sequence + 1)
				std::this_thread::yield();
		}
	}
	shm_reader.join();
	consumer.join();
	const uint64_t elapsed = now_ns() - begin;

	report("shm", samples, [](const StageSample& sample) { return sample.shm; });
	report("spsc", samples, [](const StageSample& sample) { return sample.spsc; });
	report("book_strategy", samples, [](const StageSample& sample) { return sample.book_strategy; });
	report("end_to_end", samples, [](const StageSample& sample) { return sample.end_to_end; });
	std::printf("book=%s mode=%s events=%zu throughput=%.0f event/s trades=%llu simulated_volume=%llu active_orders=%zu\n",
#ifdef WT_DENSE_ORDERBOOK
		"dense",
#else
		"reference",
#endif
		paced ? "paced" : "saturated", source.size(), static_cast<double>(source.size()) * 1e9 / elapsed,
		static_cast<unsigned long long>(sink.trades),
		static_cast<unsigned long long>(sink.volume), book.active_orders());
	return failed.load(std::memory_order_relaxed) ? 2 : 0;
}
