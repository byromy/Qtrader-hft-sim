#include "../Includes/UftStrategyDefs.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WtUftCore/ParserAdapter.h"
#include "../WtUftCore/UftStraContext.h"
#include "../WtUftCore/WtUftEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <x86intrin.h>

namespace
{
	inline std::uint64_t tsc_begin() noexcept
	{
		_mm_lfence();
		return __rdtsc();
	}

	inline std::uint64_t tsc_end() noexcept
	{
		unsigned int auxiliary = 0;
		const auto value = __rdtscp(&auxiliary);
		_mm_lfence();
		return value;
	}

	double estimate_tsc_hz()
	{
		const auto wall_begin = std::chrono::steady_clock::now();
		const auto cycle_begin = tsc_begin();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const auto cycle_end = tsc_end();
		const auto wall_end = std::chrono::steady_clock::now();
		const auto seconds = std::chrono::duration<double>(wall_end - wall_begin).count();
		return static_cast<double>(cycle_end - cycle_begin) / seconds;
	}

	std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p)
	{
		const auto index = static_cast<std::size_t>(
			std::ceil(p * static_cast<double>(sorted.size())) - 1.0);
		return sorted[std::min(index, sorted.size() - 1)];
	}

	void report(const char* name, std::vector<std::uint64_t> samples,
		std::uint64_t overhead, double tsc_hz)
	{
		for (auto& sample : samples)
			sample = sample > overhead ? sample - overhead : 0;
		std::sort(samples.begin(), samples.end());
		long double sum = 0;
		for (const auto sample : samples)
			sum += sample;
		const auto mean = static_cast<double>(sum / samples.size());
		const auto p50 = percentile(samples, 0.50);
		const auto p99 = percentile(samples, 0.99);
		const auto p999 = percentile(samples, 0.999);
		const double ns_per_cycle = 1e9 / tsc_hz;
		std::printf("%-24s mean=%7.2f cyc (%6.2f ns) p50=%llu (%5.2f ns) "
			"p99=%llu (%5.2f ns) p99.9=%llu (%6.2f ns)\n",
			name, mean, mean * ns_per_cycle,
			static_cast<unsigned long long>(p50), p50 * ns_per_cycle,
			static_cast<unsigned long long>(p99), p99 * ns_per_cycle,
			static_cast<unsigned long long>(p999), p999 * ns_per_cycle);
	}

	class BenchStrategy final : public UftStrategy
	{
	public:
		explicit BenchStrategy(const char* id) : UftStrategy(id) {}
		const char* getName() override { return "ITCHUftBench"; }
		const char* getFactName() override { return "Qtrader"; }
		void on_init(IUftStraCtx* ctx) override
		{
			ctx->stra_sub_order_details("PSX.AAPL");
			ctx->stra_sub_transactions("PSX.AAPL");
		}
		void on_order_detail(IUftStraCtx*, const char*, WTSOrdDtlData*) override
		{
			++order_callbacks;
		}
		void on_transaction(IUftStraCtx*, const char*, WTSTransData*) override
		{
			++transaction_callbacks;
		}
		std::uint64_t order_callbacks{0};
		std::uint64_t transaction_callbacks{0};
	};

	class MemoryParser final : public IParserApi
	{
	public:
		bool init(WTSVariant*) override { return true; }
		bool connect() override { return true; }
		bool disconnect() override { return true; }
		bool isConnected() override { return true; }
		void registerSpi(IParserSpi* spi) override { _spi = spi; }
		void emit(WTSOrdDtlData* data) { _spi->handleOrderDetail(data); }
		void emit(WTSTransData* data) { _spi->handleTransaction(data); }
	private:
		IParserSpi* _spi{nullptr};
	};

	template<typename Prepare, typename Invoke>
	std::vector<std::uint64_t> measure(std::uint32_t warmup,
		std::uint32_t count, Prepare prepare, Invoke invoke)
	{
		for (std::uint32_t i = 0; i < warmup; ++i)
		{
			prepare();
			invoke();
		}
		std::vector<std::uint64_t> samples(count);
		for (std::uint32_t i = 0; i < count; ++i)
		{
			prepare();
			const auto begin = tsc_begin();
			invoke();
			const auto end = tsc_end();
			samples[i] = end - begin;
		}
		return samples;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4 || argc > 6)
	{
		std::fprintf(stderr, "usage: ParserITCHUftBench <sessions.json> "
			"<commodities.json> <contracts.json> [samples] [warmup]\n");
		return 2;
	}
	const auto samples = argc > 4 ? static_cast<std::uint32_t>(
		std::strtoul(argv[4], nullptr, 10)) : 1000000U;
	const auto warmup = argc > 5 ? static_cast<std::uint32_t>(
		std::strtoul(argv[5], nullptr, 10)) : 100000U;
	if (samples == 0)
		return 2;

	WTSBaseDataMgr base_data;
	if (!base_data.loadSessions(argv[1]) ||
		!base_data.loadCommodities(argv[2]) ||
		!base_data.loadContracts(argv[3]))
		return 3;

	WtUftEngine engine;
	engine.init(nullptr, &base_data, nullptr, nullptr);
	BenchStrategy strategy("itch_uft_bench");
	auto* raw_context = new UftStraContext(&engine, "itch_uft_bench");
	raw_context->set_strategy(&strategy);
	UftContextPtr context(raw_context);
	engine.addContext(context);
	context->on_init();

	auto* parser = new MemoryParser();
	ParserAdapter adapter;
	if (!adapter.initExt("memory", parser, &engine, &base_data))
		return 4;

	WTSOrdDtlStruct order_struct{};
	wt_strcpy(order_struct.exchg, "PSX");
	wt_strcpy(order_struct.code, "AAPL");
	order_struct.trading_date = order_struct.action_date = 20190730;
	order_struct.index = 1;
	order_struct.price = 200.0;
	order_struct.volume = 100;
	order_struct.side = BDT_Buy;
	order_struct.otype = ODT_LimitPrice;
	auto* order = WTSOrdDtlData::create(order_struct);

	WTSTransStruct transaction_struct{};
	wt_strcpy(transaction_struct.exchg, "PSX");
	wt_strcpy(transaction_struct.code, "AAPL");
	transaction_struct.trading_date = transaction_struct.action_date = 20190730;
	transaction_struct.index = 1;
	transaction_struct.price = 200.0;
	transaction_struct.volume = 100;
	transaction_struct.side = BDT_Buy;
	transaction_struct.ttype = TT_Match;
	auto* transaction = WTSTransData::create(transaction_struct);

	std::vector<std::uint64_t> empty(samples);
	for (auto& value : empty)
	{
		const auto begin = tsc_begin();
		const auto end = tsc_end();
		value = end - begin;
	}
	std::sort(empty.begin(), empty.end());
	const auto overhead = percentile(empty, 0.50);
	const auto tsc_hz = estimate_tsc_hz();

	auto engine_order = measure(warmup, samples, [&]() {
		order->setCode("PSX.AAPL");
	}, [&]() { engine.handle_push_order_detail(order); });
	auto adapter_order = measure(warmup, samples, [&]() {
		order->setCode("AAPL");
	}, [&]() { parser->emit(order); });
	auto engine_transaction = measure(warmup, samples, [&]() {
		transaction->setCode("PSX.AAPL");
	}, [&]() { engine.handle_push_transaction(transaction); });
	auto adapter_transaction = measure(warmup, samples, [&]() {
		transaction->setCode("AAPL");
	}, [&]() { parser->emit(transaction); });

	std::printf("samples=%u warmup=%u timing=LFENCE+RDTSC/RDTSCP+LFENCE "
		"median_overhead=%llu_cycles tsc=%.3f_MHz\n",
		samples, warmup, static_cast<unsigned long long>(overhead), tsc_hz / 1e6);
	report("engine_order_detail", std::move(engine_order), overhead, tsc_hz);
	report("adapter_order_detail", std::move(adapter_order), overhead, tsc_hz);
	report("engine_transaction", std::move(engine_transaction), overhead, tsc_hz);
	report("adapter_transaction", std::move(adapter_transaction), overhead, tsc_hz);
	std::printf("order_callbacks=%llu transaction_callbacks=%llu\n",
		static_cast<unsigned long long>(strategy.order_callbacks),
		static_cast<unsigned long long>(strategy.transaction_callbacks));

	order->release();
	transaction->release();
	adapter.release();
	return 0;
}
