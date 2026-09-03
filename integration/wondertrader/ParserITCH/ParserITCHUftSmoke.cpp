#include "../Includes/UftStrategyDefs.h"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WtUftCore/ParserAdapter.h"
#include "../WtUftCore/UftStraContext.h"
#include "../WtUftCore/WtUftEngine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{
	class CountingStrategy final : public UftStrategy
	{
	public:
		CountingStrategy(const char* id, std::string code,
			std::uint64_t expected_orders, std::uint64_t expected_transactions)
			: UftStrategy(id), _code(std::move(code)),
			  _expected_orders(expected_orders),
			  _expected_transactions(expected_transactions)
		{
		}

		const char* getName() override { return "CountITCH"; }
		const char* getFactName() override { return "QtraderUftSmoke"; }

		void on_init(IUftStraCtx* ctx) override
		{
			ctx->stra_sub_order_details(_code.c_str());
			ctx->stra_sub_transactions(_code.c_str());
		}

		void on_order_detail(IUftStraCtx*, const char* code,
			WTSOrdDtlData*) override
		{
			if (_code != code)
				++_wrong_code;
			++_orders;
			check_complete();
		}

		void on_transaction(IUftStraCtx*, const char* code,
			WTSTransData*) override
		{
			if (_code != code)
				++_wrong_code;
			++_transactions;
			check_complete();
		}

		bool complete() const noexcept
		{
			return _complete.load(std::memory_order_acquire);
		}

		std::uint64_t orders() const noexcept { return _orders.load(); }
		std::uint64_t transactions() const noexcept
		{
			return _transactions.load();
		}
		std::uint64_t wrong_code() const noexcept { return _wrong_code.load(); }

	private:
		void check_complete() noexcept
		{
			if (_orders.load(std::memory_order_relaxed) == _expected_orders &&
				_transactions.load(std::memory_order_relaxed) ==
					_expected_transactions)
			{
				_complete.store(true, std::memory_order_release);
			}
		}

		std::string _code;
		std::uint64_t _expected_orders;
		std::uint64_t _expected_transactions;
		std::atomic<std::uint64_t> _orders{0};
		std::atomic<std::uint64_t> _transactions{0};
		std::atomic<std::uint64_t> _wrong_code{0};
		std::atomic<bool> _complete{false};
	};

	std::uint64_t parse_u64(const char* text)
	{
		char* end = nullptr;
		const auto value = std::strtoull(text, &end, 10);
		if (end == text || *end != '\0')
			throw std::invalid_argument("invalid integer argument");
		return value;
	}
}

int main(int argc, char** argv)
{
	if (argc != 9)
	{
		std::cerr << "usage: ParserITCHUftSmoke <itch-file> <symbol> <date> "
			"<sessions.json> <commodities.json> <contracts.json> "
			"<expected-orders> <expected-transactions>\n";
		return 2;
	}

	try
	{
		const std::string symbol = argv[2];
		const std::string std_code = "PSX." + symbol;
		const auto trading_date = static_cast<std::uint32_t>(parse_u64(argv[3]));
		const auto expected_orders = parse_u64(argv[7]);
		const auto expected_transactions = parse_u64(argv[8]);

		WTSBaseDataMgr base_data;
		if (!base_data.loadSessions(argv[4]) ||
			!base_data.loadCommodities(argv[5]) ||
			!base_data.loadContracts(argv[6]) ||
			base_data.getContract(symbol.c_str(), "PSX") == nullptr)
		{
			std::cerr << "failed to load PSX base data\n";
			return 3;
		}

		WtUftEngine engine;
		engine.init(nullptr, &base_data, nullptr, nullptr);

		CountingStrategy strategy("itch_uft_smoke", std_code,
			expected_orders, expected_transactions);
		auto* raw_context = new UftStraContext(&engine, "itch_uft_smoke");
		raw_context->set_strategy(&strategy);
		UftContextPtr context(raw_context);
		engine.addContext(context);
		context->on_init();

		WTSVariant* config = WTSVariant::createObject();
		config->append("module", "ParserITCH");
		config->append("path", argv[1]);
		config->append("symbol", symbol.c_str());
		config->append("date", trading_date);
		config->append("filter", "PSX");
		config->append("code", std_code.c_str());
		config->append("gpsize", static_cast<std::uint32_t>(100000));

		ParserAdapter adapter;
		if (!adapter.init("itch", config, &engine, &base_data) ||
			!adapter.run())
		{
			config->release();
			std::cerr << "failed to start ParserITCH through ParserAdapter\n";
			return 4;
		}

		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(30);
		while (!strategy.complete() &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		if (strategy.complete())
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		adapter.release();
		config->release();

		std::cout << "standard_code=" << std_code
			<< " orders=" << strategy.orders()
			<< " transactions=" << strategy.transactions()
			<< " wrong_code=" << strategy.wrong_code() << '\n';

		if (!strategy.complete() || strategy.wrong_code() != 0 ||
			strategy.orders() != expected_orders ||
			strategy.transactions() != expected_transactions)
			return 5;
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 6;
	}
}
