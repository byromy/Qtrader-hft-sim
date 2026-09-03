#include "UftStrategyDefs.h"
#include "WTSDataDef.hpp"
#include "WTSBaseDataMgr.h"
#include "ParserAdapter.h"
#include "UftStraContext.h"
#include "WtUftEngine.h"

#include "QUftOpaque.hpp"

#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/NasdaqItchWtsAdapter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <x86intrin.h>
#include <sys/resource.h>

namespace
{
	constexpr std::size_t kMaximumMessageSize = 64;
	constexpr double kMinimumThroughputSeconds = 5.0;

#if defined(__GNUC__) || defined(__clang__)
#define QTRADER_NOINLINE __attribute__((noinline))
#else
#define QTRADER_NOINLINE
#endif

	struct RawMessage
	{
		std::uint8_t size{0};
		std::array<std::uint8_t, kMaximumMessageSize> bytes{};
	};

	struct Corpus
	{
		std::uint16_t locator{0};
		std::vector<RawMessage> messages;
	};

	inline std::uint64_t tsc_begin() noexcept
	{
		_mm_lfence();
		return __rdtsc();
	}

	inline std::uint64_t tsc_end(unsigned int* auxiliary = nullptr) noexcept
	{
		unsigned int cpu = 0;
		const auto value = __rdtscp(&cpu);
		_mm_lfence();
		if (auxiliary != nullptr)
			*auxiliary = cpu;
		return value;
	}

	double estimate_tsc_hz()
	{
		const auto wall_begin = std::chrono::steady_clock::now();
		const auto cycle_begin = tsc_begin();
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		const auto cycle_end = tsc_end();
		const auto wall_end = std::chrono::steady_clock::now();
		return static_cast<double>(cycle_end - cycle_begin) /
			std::chrono::duration<double>(wall_end - wall_begin).count();
	}

	std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
		double probability)
	{
		const auto index = static_cast<std::size_t>(
			std::ceil(probability * static_cast<double>(sorted.size())) - 1.0);
		return sorted[std::min(index, sorted.size() - 1)];
	}

	struct Summary
	{
		double mean_ns{0};
		double p50_ns{0};
		double p99_ns{0};
		double p999_ns{0};
		double max_ns{0};
	};

	Summary summarize(std::vector<std::uint64_t> samples,
		std::uint64_t overhead, double tsc_hz)
	{
		long double sum = 0;
		for (auto& sample : samples)
		{
			sample = sample > overhead ? sample - overhead : 0;
			sum += sample;
		}
		std::sort(samples.begin(), samples.end());
		const auto ns_per_cycle = 1e9 / tsc_hz;
		return {static_cast<double>(sum / samples.size()) * ns_per_cycle,
			percentile(samples, .50) * ns_per_cycle,
			percentile(samples, .99) * ns_per_cycle,
			percentile(samples, .999) * ns_per_cycle,
			samples.back() * ns_per_cycle};
	}

	std::string_view trimmed_symbol(const std::uint8_t* bytes)
	{
		std::size_t length = 8;
		while (length != 0 && bytes[length - 1] == ' ')
			--length;
		return {reinterpret_cast<const char*>(bytes), length};
	}

	Corpus load_corpus(const char* path, const std::string& symbol)
	{
		Corpus corpus;
		corpus.messages.reserve(750000);
		qtrader::itch50::scan_historical_file(path,
			[&](const std::uint8_t* message, std::size_t size)
			{
				if (message[0] == 'R' && size == 39 &&
					trimmed_symbol(message + 11) == symbol)
					corpus.locator = qtrader::itch50::read_u16(message + 1);
				if (corpus.locator != 0 && size >= 11 &&
					qtrader::itch50::read_u16(message + 1) == corpus.locator)
				{
					if (size > kMaximumMessageSize)
						throw std::runtime_error("ITCH message exceeds corpus slot");
					RawMessage copy;
					copy.size = static_cast<std::uint8_t>(size);
					std::memcpy(copy.bytes.data(), message, size);
					corpus.messages.push_back(copy);
				}
			});
		if (corpus.locator == 0 || corpus.messages.empty())
			throw std::runtime_error("symbol not found in ITCH file");
		return corpus;
	}

	std::uint32_t action_time(std::uint64_t timestamp_ns) noexcept
	{
		const auto milliseconds = timestamp_ns / 1'000'000ULL;
		return static_cast<std::uint32_t>(
			(milliseconds / 3'600'000ULL) * 10'000'000ULL +
			((milliseconds / 60'000ULL) % 60ULL) * 100'000ULL +
			((milliseconds / 1'000ULL) % 60ULL) * 1'000ULL +
			milliseconds % 1'000ULL);
	}

	class SemanticFingerprint
	{
	public:
		void add_order(const wtp::WTSOrdDtlStruct& event) noexcept
		{
			add(1);
			add(static_cast<std::uint64_t>(event.index));
			add(static_cast<std::uint64_t>(std::llround(event.price * 10000.0)));
			add(event.volume);
			add(event.side == BDT_Buy ? 1 : event.side == BDT_Sell ? 2 : 0);
			add(event.otype == ODT_LimitPrice ? 1 : 0);
			add(event.action_time);
		}

		void add_order(const qtrader::UftOrderDetail& event) noexcept
		{
			add(1);
			add(event.order_id);
			add(static_cast<std::uint64_t>(event.price_ticks));
			add(event.quantity);
			add(event.side == qtrader::UftSide::Buy ? 1 :
				event.side == qtrader::UftSide::Sell ? 2 : 0);
			add(event.order_type == qtrader::UftOrderType::Limit ? 1 : 0);
			add(action_time(event.exchange_time_ns));
		}

		void add_transaction(const wtp::WTSTransStruct& event) noexcept
		{
			add(2);
			add(static_cast<std::uint64_t>(event.index));
			add(static_cast<std::uint64_t>(std::llround(event.price * 10000.0)));
			add(event.volume);
			add(event.side == BDT_Buy ? 1 : event.side == BDT_Sell ? 2 : 0);
			add(event.ttype == TT_Match ? 1 : event.ttype == TT_Cancel ? 2 :
				event.ttype == qtrader::itch50::kWtsTradeBust ? 3 : 0);
			add(static_cast<std::uint64_t>(event.askorder));
			add(static_cast<std::uint64_t>(event.bidorder));
			add(event.action_time);
		}

		void add_transaction(const qtrader::UftTransaction& event) noexcept
		{
			add(2);
			add(event.transaction_id);
			add(static_cast<std::uint64_t>(event.price_ticks));
			add(event.quantity);
			add(event.side == qtrader::UftSide::Buy ? 1 :
				event.side == qtrader::UftSide::Sell ? 2 : 0);
			add(event.transaction_type == qtrader::UftTransactionType::Trade ? 1 :
				event.transaction_type == qtrader::UftTransactionType::Cancel ? 2 :
				event.transaction_type == qtrader::UftTransactionType::TradeBust ? 3 : 0);
			add(event.ask_order_id);
			add(event.bid_order_id);
			add(action_time(event.exchange_time_ns));
		}

		void reset() noexcept
		{
			digest_ = 1469598103934665603ULL;
			fields_ = 0;
		}
		std::uint64_t digest() const noexcept { return digest_; }
		std::uint64_t fields() const noexcept { return fields_; }

	private:
		void add(std::uint64_t value) noexcept
		{
			// Ordered field hash: changing an event, field, or position changes the
			// digest. This is deliberately not a commutative checksum.
			digest_ ^= value + 0x9e3779b97f4a7c15ULL + fields_;
			digest_ *= 1099511628211ULL;
			digest_ ^= digest_ >> 32;
			++fields_;
		}

		std::uint64_t digest_{1469598103934665603ULL};
		std::uint64_t fields_{0};
	};

	struct StrategyAudit
	{
		SemanticFingerprint fingerprint;
		std::uint64_t order_callbacks{0};
		std::uint64_t transaction_callbacks{0};
		std::uint64_t first_arrival_cycles{0};
		std::uint64_t consumed{0};
		bool audit_enabled{true};
		bool latency_probe_enabled{false};
		bool have_arrival{false};

		void reset(bool enable_audit = true,
			bool enable_latency_probe = false) noexcept
		{
			fingerprint.reset();
			order_callbacks = transaction_callbacks = 0;
			first_arrival_cycles = 0;
			consumed = 0;
			audit_enabled = enable_audit;
			latency_probe_enabled = enable_latency_probe;
			have_arrival = false;
		}

		void begin_message() noexcept { have_arrival = false; }

		void record_arrival() noexcept
		{
			if (latency_probe_enabled && !have_arrival)
			{
				first_arrival_cycles = tsc_end();
				have_arrival = true;
			}
		}
		std::uint64_t callbacks() const noexcept
		{
			return order_callbacks + transaction_callbacks;
		}
	};

	class ChainStrategy final : public UftStrategy
	{
	public:
		ChainStrategy(const char* id, std::string code)
			: UftStrategy(id), code_(std::move(code)) {}
		const char* getName() override { return "ITCHUftFormalBench"; }
		const char* getFactName() override { return "Qtrader"; }
		void on_init(IUftStraCtx* context) override
		{
			context->stra_sub_order_details(code_.c_str());
			context->stra_sub_transactions(code_.c_str());
		}
		void on_order_detail(IUftStraCtx*, const char*, WTSOrdDtlData* data) override
		{
			audit.record_arrival();
			const auto& event = data->getOrdDtlStruct();
			if (audit.audit_enabled)
				audit.fingerprint.add_order(event);
			audit.consumed ^= static_cast<std::uint64_t>(event.index) +
				audit.order_callbacks;
			++audit.order_callbacks;
		}
		void on_transaction(IUftStraCtx*, const char*, WTSTransData* data) override
		{
			audit.record_arrival();
			const auto& event = data->getTransStruct();
			if (audit.audit_enabled)
				audit.fingerprint.add_transaction(event);
			audit.consumed ^= static_cast<std::uint64_t>(event.index) +
				audit.transaction_callbacks;
			++audit.transaction_callbacks;
		}
		StrategyAudit audit;
	private:
		std::string code_;
	};

	class MemoryParser final : public IParserApi
	{
	public:
		bool init(WTSVariant*) override { return true; }
		bool connect() override { return true; }
		bool disconnect() override { return true; }
		bool isConnected() override { return true; }
		void registerSpi(IParserSpi* spi) override { spi_ = spi; }
		void emit(WTSOrdDtlData* data) { spi_->handleOrderDetail(data); }
		void emit(WTSTransData* data) { spi_->handleTransaction(data); }
	private:
		IParserSpi* spi_{nullptr};
	};

	class WonderTraderSink
	{
	public:
		explicit WonderTraderSink(MemoryParser& parser) : parser_(parser) {}
		void on_order_detail(const wtp::WTSOrdDtlStruct& value)
		{
			auto copy = value;
			auto* data = WTSOrdDtlData::create(copy);
			parser_.emit(data);
			data->release();
		}
		void on_transaction(const wtp::WTSTransStruct& value)
		{
			auto copy = value;
			auto* data = WTSTransData::create(copy);
			parser_.emit(data);
			data->release();
		}
	private:
		MemoryParser& parser_;
	};

	struct WonderTraderChain
	{
		WonderTraderChain(std::uint32_t date, std::uint16_t locator,
			const std::string& symbol, WonderTraderSink& sink_value)
			: adapter(date, locator, symbol), sink(sink_value) {}
		qtrader::itch50::PsxWtsAdapter adapter;
		WonderTraderSink& sink;
	};

	QTRADER_NOINLINE void wt_message(WonderTraderChain* chain,
		const std::uint8_t* message, std::size_t size) noexcept
	{
		chain->adapter.on_message(message, size, chain->sink);
	}

	struct QStrategy
	{
		StrategyAudit audit;
		static void order(void* self,
			const qtrader::UftOrderDetail* event) noexcept
		{
			auto& value = static_cast<QStrategy*>(self)->audit;
			value.record_arrival();
			if (value.audit_enabled)
				value.fingerprint.add_order(*event);
			value.consumed ^= event->order_id + value.order_callbacks;
			++value.order_callbacks;
		}
		static void transaction(void* self,
			const qtrader::UftTransaction* event) noexcept
		{
			auto& value = static_cast<QStrategy*>(self)->audit;
			value.record_arrival();
			if (value.audit_enabled)
				value.fingerprint.add_transaction(*event);
			value.consumed ^= event->transaction_id + value.transaction_callbacks;
			++value.transaction_callbacks;
		}
	};

	struct CommonStats
	{
		std::uint64_t order_details{0};
		std::uint64_t transactions{0};
		std::uint64_t matches{0};
		std::uint64_t cancels{0};
		std::uint64_t trade_busts{0};
		std::uint64_t duplicate_matches{0};
		std::uint64_t non_printable{0};
		std::uint64_t broken_without_output{0};
		std::uint64_t unknown_broken{0};
		std::uint64_t errors{0};
		std::uint64_t peak_active_orders{0};
	};

	CommonStats common(const qtrader::itch50::WtsAdapterStats& value) noexcept
	{
		return {value.order_details, value.transactions, value.matches,
			value.cancels, value.trade_busts,
			value.duplicate_matches_suppressed,
			value.non_printable_suppressed,
			value.broken_trades_without_output, value.unknown_broken_trades,
			value.errors, value.peak_active_orders};
	}

	CommonStats common(const QUftOpaqueStats& value) noexcept
	{
		return {value.order_details, value.transactions, value.matches,
			value.cancels, value.trade_busts,
			value.duplicate_matches_suppressed,
			value.non_printable_suppressed,
			value.broken_trades_without_output, value.unknown_broken_trades,
			value.errors + value.capacity_exhaustions, value.peak_active_orders};
	}

	bool same(const CommonStats& left, const CommonStats& right) noexcept
	{
		return left.order_details == right.order_details &&
			left.transactions == right.transactions &&
			left.matches == right.matches && left.cancels == right.cancels &&
			left.trade_busts == right.trade_busts &&
			left.duplicate_matches == right.duplicate_matches &&
			left.non_printable == right.non_printable &&
			left.broken_without_output == right.broken_without_output &&
			left.unknown_broken == right.unknown_broken &&
			left.errors == right.errors &&
			left.peak_active_orders == right.peak_active_orders;
	}

	struct TimedResult
	{
		std::vector<std::uint64_t> all_messages;
		std::vector<std::uint64_t> callback_arrivals;
		CommonStats stats;
		std::uint64_t migrations{0};
		std::size_t active_orders{0};
	};

	template<typename State, typename Process, typename Counter,
		typename BeginMessage, typename Arrival, typename Stats, typename Active>
	TimedResult replay_timed(const Corpus& corpus, State* state, Process process,
		Counter callback_count, BeginMessage begin_message, Arrival arrival,
		Stats stats, Active active, int expected_cpu)
	{
		TimedResult result;
		result.all_messages.reserve(corpus.messages.size());
		result.callback_arrivals.reserve(corpus.messages.size());
		for (const auto& message : corpus.messages)
		{
			const auto before = callback_count();
			begin_message();
			const auto begin = tsc_begin();
			process(state, message.bytes.data(), message.size);
			unsigned int end_cpu = 0;
			const auto end = tsc_end(&end_cpu);
			const auto elapsed = end - begin;
			result.all_messages.push_back(elapsed);
			if (callback_count() != before)
				result.callback_arrivals.push_back(arrival() - begin);
			if (end_cpu != static_cast<unsigned int>(expected_cpu))
				++result.migrations;
		}
		result.stats = common(stats());
		result.active_orders = active();
		return result;
	}

	struct ThroughputResult
	{
		double seconds{0};
		std::uint64_t passes{0};
		std::uint64_t inputs{0};
		std::uint64_t outputs{0};
		std::uint64_t verified_callbacks{0};
	};

	ThroughputResult throughput_wt(const Corpus& corpus, std::uint32_t date,
		const std::string& symbol, WonderTraderSink& sink,
		ChainStrategy& strategy, const CommonStats& expected)
	{
		ThroughputResult result;
		while (result.seconds < kMinimumThroughputSeconds)
		{
			strategy.audit.reset(false, false);
			WonderTraderChain chain(date, corpus.locator, symbol, sink);
			const auto begin = std::chrono::steady_clock::now();
			for (const auto& message : corpus.messages)
				wt_message(&chain, message.bytes.data(), message.size);
			const auto end = std::chrono::steady_clock::now();
			const auto stats = common(chain.adapter.stats());
			const auto outputs = stats.order_details + stats.transactions;
			if (!same(stats, expected) || chain.adapter.active_orders() != 0 ||
				strategy.audit.order_callbacks != stats.order_details ||
				strategy.audit.transaction_callbacks != stats.transactions ||
				strategy.audit.callbacks() != outputs)
				throw std::runtime_error("WonderTrader sustained replay failed");
			result.seconds += std::chrono::duration<double>(end - begin).count();
			++result.passes;
			result.inputs += corpus.messages.size();
			result.outputs += outputs;
			result.verified_callbacks += strategy.audit.callbacks();
		}
		return result;
	}

	struct QFactory
	{
		std::uint16_t locator{0};
		std::uint32_t instrument_id{0};
		std::uint32_t strategy_id{0};
		const char* standard_code{nullptr};
		std::size_t order_capacity{0};
		std::size_t match_capacity{0};
		QStrategy* strategy{nullptr};

		std::unique_ptr<QUftOpaque, decltype(&quft_destroy)> create() const
		{
			return {quft_create(locator, instrument_id, strategy_id,
				standard_code, order_capacity, match_capacity, strategy,
				&QStrategy::order, &QStrategy::transaction), &quft_destroy};
		}
	};

	ThroughputResult throughput_q(const Corpus& corpus, const QFactory& factory,
		const CommonStats& expected)
	{
		ThroughputResult result;
		while (result.seconds < kMinimumThroughputSeconds)
		{
			factory.strategy->audit.reset(false, false);
			auto runner = factory.create();
			if (!runner)
				throw std::runtime_error("Qtrader runner creation failed");
			const auto begin = std::chrono::steady_clock::now();
			for (const auto& message : corpus.messages)
				quft_message(runner.get(), message.bytes.data(), message.size);
			const auto end = std::chrono::steady_clock::now();
			const auto raw_stats = quft_stats(runner.get());
			const auto stats = common(raw_stats);
			const auto outputs = stats.order_details + stats.transactions;
			if (!same(stats, expected) || quft_active_orders(runner.get()) != 0 ||
				raw_stats.order_events_delivered != stats.order_details ||
				raw_stats.transaction_events_delivered != stats.transactions ||
				raw_stats.callback_deliveries != outputs ||
				raw_stats.undelivered_events != 0 ||
				factory.strategy->audit.order_callbacks != stats.order_details ||
				factory.strategy->audit.transaction_callbacks != stats.transactions ||
				factory.strategy->audit.callbacks() != outputs)
				throw std::runtime_error("Qtrader sustained replay failed");
			result.seconds += std::chrono::duration<double>(end - begin).count();
			++result.passes;
			result.inputs += corpus.messages.size();
			result.outputs += outputs;
			result.verified_callbacks += factory.strategy->audit.callbacks();
		}
		return result;
	}

	std::uint64_t parse_u64(const char* text)
	{
		char* end = nullptr;
		const auto value = std::strtoull(text, &end, 10);
		if (end == text || *end != '\0')
			throw std::invalid_argument("invalid integer argument");
		return value;
	}
}

int main(int argc, char** argv) try
{
	if (argc < 7 || argc > 8)
	{
		std::fprintf(stderr, "usage: ParserITCHUftFormalBench <itch-file> "
			"<symbol> <date> <sessions.json> <commodities.json> <contracts.json> "
			"[WTQ|QWT]\n");
		return 2;
	}
	const std::string symbol = argv[2];
	const std::string standard_code = "PSX." + symbol;
	const auto trading_date = static_cast<std::uint32_t>(parse_u64(argv[3]));
	const std::string path_order = argc > 7 ? argv[7] : "WTQ";
	if (path_order != "WTQ" && path_order != "QWT")
		throw std::invalid_argument("path order must be WTQ or QWT");
	const auto expected_cpu = ::sched_getcpu();
	if (expected_cpu < 0)
		throw std::runtime_error("sched_getcpu failed");

	const auto corpus = load_corpus(argv[1], symbol);
	WTSBaseDataMgr base_data;
	if (!base_data.loadSessions(argv[4]) || !base_data.loadCommodities(argv[5]) ||
		!base_data.loadContracts(argv[6]) ||
		base_data.getContract(symbol.c_str(), "PSX") == nullptr)
		return 3;

	WtUftEngine wt_engine;
	wt_engine.init(nullptr, &base_data, nullptr, nullptr);
	ChainStrategy wt_strategy("itch_uft_formal", standard_code);
	auto* raw_context = new UftStraContext(&wt_engine, "itch_uft_formal");
	raw_context->set_strategy(&wt_strategy);
	UftContextPtr context(raw_context);
	wt_engine.addContext(context);
	context->on_init();
	auto* memory_parser = new MemoryParser();
	ParserAdapter parser_adapter;
	if (!parser_adapter.initExt("memory", memory_parser, &wt_engine, &base_data))
		return 4;
	WonderTraderSink wt_sink(*memory_parser);

	// Untimed audit replay: derive capacities, then prove both paths produce the
	// same ordered business fields before measuring minimal callbacks.
	wt_strategy.audit.reset(true, false);
	WonderTraderChain wt_warm(trading_date, corpus.locator, symbol, wt_sink);
	for (const auto& message : corpus.messages)
		wt_message(&wt_warm, message.bytes.data(), message.size);
	const auto expected_stats = common(wt_warm.adapter.stats());
	if (expected_stats.errors != 0 || wt_warm.adapter.active_orders() != 0)
		throw std::runtime_error("WonderTrader warmup validation failed");
	const auto order_capacity = std::max<std::size_t>(1024,
		static_cast<std::size_t>(expected_stats.peak_active_orders) * 4);
	const auto match_capacity = std::max<std::size_t>(1024,
		static_cast<std::size_t>(expected_stats.matches +
			expected_stats.non_printable) * 2);

	QStrategy q_strategy;
	const auto instrument_id = 1U + static_cast<std::uint32_t>(
		corpus.locator % (quft_instrument_capacity() - 1U));
	const auto strategy_id = 1U + static_cast<std::uint32_t>(
		(corpus.locator * 17U) % (quft_strategy_capacity() - 1U));
	QFactory q_factory{corpus.locator, instrument_id, strategy_id,
		standard_code.c_str(), order_capacity, match_capacity, &q_strategy};
	q_strategy.audit.reset(true, false);
	auto q_warm = q_factory.create();
	if (!q_warm)
		throw std::runtime_error("Qtrader warmup creation failed");
	for (const auto& message : corpus.messages)
		quft_message(q_warm.get(), message.bytes.data(), message.size);
	const auto q_audit_stats = quft_stats(q_warm.get());
	const auto expected_outputs = expected_stats.order_details +
		expected_stats.transactions;
	const bool audit_valid = expected_stats.errors == 0 &&
		expected_stats.unknown_broken == 0 &&
		same(common(q_audit_stats), expected_stats) &&
		quft_active_orders(q_warm.get()) == 0 &&
		wt_strategy.audit.order_callbacks == expected_stats.order_details &&
		wt_strategy.audit.transaction_callbacks == expected_stats.transactions &&
		q_strategy.audit.order_callbacks == expected_stats.order_details &&
		q_strategy.audit.transaction_callbacks == expected_stats.transactions &&
		q_audit_stats.order_events_delivered == expected_stats.order_details &&
		q_audit_stats.transaction_events_delivered == expected_stats.transactions &&
		q_audit_stats.callback_deliveries == expected_outputs &&
		q_audit_stats.undelivered_events == 0 &&
		wt_strategy.audit.fingerprint.fields() ==
			q_strategy.audit.fingerprint.fields() &&
		wt_strategy.audit.fingerprint.digest() ==
			q_strategy.audit.fingerprint.digest();
	if (!audit_valid)
		throw std::runtime_error("Qtrader warmup validation failed");
	const auto q_table_memory = quft_startup_table_memory(q_warm.get());
	const auto q_static_memory = quft_runner_static_memory();
	q_warm.reset();

	TimedResult wt_timed;
	TimedResult q_timed;
	QUftOpaqueStats q_timed_raw_stats{};
	auto time_wt = [&]
	{
		wt_strategy.audit.reset(false, true);
		WonderTraderChain chain(trading_date, corpus.locator, symbol, wt_sink);
		wt_timed = replay_timed(corpus, &chain, &wt_message,
			[&] { return wt_strategy.audit.callbacks(); },
			[&] { wt_strategy.audit.begin_message(); },
			[&] { return wt_strategy.audit.first_arrival_cycles; },
			[&] { return chain.adapter.stats(); },
			[&] { return chain.adapter.active_orders(); }, expected_cpu);
	};
	auto time_q = [&]
	{
		q_strategy.audit.reset(false, true);
		auto runner = q_factory.create();
		if (!runner)
			throw std::runtime_error("Qtrader timed creation failed");
		q_timed = replay_timed(corpus, runner.get(), &quft_message,
			[&] { return q_strategy.audit.callbacks(); },
			[&] { q_strategy.audit.begin_message(); },
			[&] { return q_strategy.audit.first_arrival_cycles; },
			[&] { return quft_stats(runner.get()); },
			[&] { return quft_active_orders(runner.get()); }, expected_cpu);
		q_timed_raw_stats = quft_stats(runner.get());
	};
	if (path_order == "QWT") { time_q(); time_wt(); }
	else { time_wt(); time_q(); }

	const bool timed_valid = audit_valid && same(wt_timed.stats, q_timed.stats) &&
		same(wt_timed.stats, expected_stats) && wt_timed.active_orders == 0 &&
		q_timed.active_orders == 0 && wt_timed.migrations == 0 &&
		q_timed.migrations == 0 &&
		wt_strategy.audit.order_callbacks == q_strategy.audit.order_callbacks &&
		wt_strategy.audit.transaction_callbacks ==
			q_strategy.audit.transaction_callbacks &&
		wt_strategy.audit.consumed == q_strategy.audit.consumed &&
		q_timed_raw_stats.order_events_delivered == expected_stats.order_details &&
		q_timed_raw_stats.transaction_events_delivered == expected_stats.transactions &&
		q_timed_raw_stats.callback_deliveries == expected_outputs &&
		q_timed_raw_stats.undelivered_events == 0;

	std::vector<std::uint64_t> empty(corpus.messages.size());
	for (auto& sample : empty)
	{
		const auto begin = tsc_begin();
		sample = tsc_end() - begin;
	}
	std::sort(empty.begin(), empty.end());
	const auto overhead = percentile(empty, .50);
	const auto tsc_hz = estimate_tsc_hz();
	auto report_latency = [&](const char* scope, const char* framework,
		const std::vector<std::uint64_t>& samples, std::uint64_t migrations)
	{
		const auto value = summarize(samples, overhead, tsc_hz);
		std::printf("CSV,uft_latency,%s,%s,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%llu\n",
			scope, framework, samples.size(), value.mean_ns, value.p50_ns,
			value.p99_ns, value.p999_ns, value.max_ns,
			static_cast<unsigned long long>(migrations));
	};
	report_latency("input_round_trip", "WonderTrader", wt_timed.all_messages,
		wt_timed.migrations);
	report_latency("input_round_trip", "Qtrader-UFT", q_timed.all_messages,
		q_timed.migrations);
	report_latency("first_callback_arrival", "WonderTrader",
		wt_timed.callback_arrivals, wt_timed.migrations);
	report_latency("first_callback_arrival", "Qtrader-UFT",
		q_timed.callback_arrivals, q_timed.migrations);

	ThroughputResult wt_throughput;
	ThroughputResult q_throughput;
	if (path_order == "QWT")
	{
		q_throughput = throughput_q(corpus, q_factory, expected_stats);
		wt_throughput = throughput_wt(corpus, trading_date, symbol, wt_sink,
			wt_strategy, expected_stats);
	}
	else
	{
		wt_throughput = throughput_wt(corpus, trading_date, symbol, wt_sink,
			wt_strategy, expected_stats);
		q_throughput = throughput_q(corpus, q_factory, expected_stats);
	}
	auto report_throughput = [](const char* framework,
		const ThroughputResult& result)
	{
		std::printf("CSV,uft_throughput,%s,%llu,%llu,%.6f,%.6f,%llu,%.6f,%llu\n",
			framework, static_cast<unsigned long long>(result.inputs),
			static_cast<unsigned long long>(result.outputs),
			result.inputs / result.seconds / 1e6,
			result.outputs / result.seconds / 1e6,
			static_cast<unsigned long long>(result.passes), result.seconds,
			static_cast<unsigned long long>(result.verified_callbacks));
	};
	report_throughput("WonderTrader", wt_throughput);
	report_throughput("Qtrader-UFT", q_throughput);

	const bool throughput_valid =
		wt_throughput.verified_callbacks == wt_throughput.outputs &&
		q_throughput.verified_callbacks == q_throughput.outputs;
	struct rusage usage{};
	(void)::getrusage(RUSAGE_SELF, &usage);
	const auto corpus_memory = corpus.messages.capacity() * sizeof(RawMessage);
	const auto sample_memory =
		(wt_timed.all_messages.capacity() + wt_timed.callback_arrivals.capacity() +
		 q_timed.all_messages.capacity() + q_timed.callback_arrivals.capacity() +
		 empty.capacity()) * sizeof(std::uint64_t);
	const bool correct = timed_valid && throughput_valid;
	std::printf("validation=%s benchmark=uft-production-v3 path_order=%s cpu=%d "
		"target_messages=%zu orders=%llu transactions=%llu "
		"trade_busts=%llu overhead=%llu_cycles tsc=%.3f_MHz "
		"q_static_memory=%zu_bytes q_table_memory=%zu_bytes "
		"corpus_memory=%zu_bytes sample_memory=%zu_bytes peak_rss=%ld_KiB "
		"audit=%s throughput_delivery=%s minimum_throughput_seconds=%.1f\n",
		correct ? "PASS" : "FAIL", path_order.c_str(), expected_cpu,
		corpus.messages.size(),
		static_cast<unsigned long long>(expected_stats.order_details),
		static_cast<unsigned long long>(expected_stats.transactions),
		static_cast<unsigned long long>(expected_stats.trade_busts),
		static_cast<unsigned long long>(overhead), tsc_hz / 1e6,
		q_static_memory, q_table_memory, corpus_memory, sample_memory,
		usage.ru_maxrss, audit_valid ? "PASS" : "FAIL",
		throughput_valid ? "PASS" : "FAIL", kMinimumThroughputSeconds);
	parser_adapter.release();
	return correct ? 0 : 6;
}
catch (const std::exception& error)
{
	std::fprintf(stderr, "error: %s\n", error.what());
	return 1;
}
