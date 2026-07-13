/*!
 * /file WtUftRunner.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */
#include "HftLatencyTool.h"
#include "../WtCore/HftStraContext.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/IParserApi.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CpuHelper.hpp"

#ifdef WT_HFT_BENCH
#include "HftBenchmark.hpp"
#endif


USING_NS_WTP;

void test_hft()
{
	hft::HftLatencyTool runner;
	runner.init();

	runner.run();
}

namespace hft
{
#ifdef WT_HFT_BENCH
	static hftbench::Collector* g_collector = nullptr;
#endif

	inline double checkValid(double x)
	{
		return ((x == DBL_MAX || x == FLT_MAX) ? 0.0 : x);
	}


	inline uint32_t strToTime(const char* strTime)
	{
		static char str[10] = { 0 };
		const char *pos = strTime;
		int idx = 0;
		auto len = strlen(strTime);
		for (std::size_t i = 0; i < len; i++)
		{
			if (strTime[i] != ':')
			{
				str[idx] = strTime[i];
				idx++;
			}
		}
		str[idx] = '\0';

		return strtoul(str, NULL, 10);
	}

	class TestParser : public IParserApi
	{
	public:
		virtual bool init(WTSVariant*) override { return true; }
		virtual bool connect() override { return true; }
		virtual bool isConnected() override { return true; }

#ifdef WT_HFT_BENCH
		void	run(uint32_t times, uint32_t warmup)
#else
		void	run(uint32_t times)
#endif
		{
			srand(time(NULL));
			TimeUtils::Ticker ticker;
#ifdef WT_HFT_BENCH
			hftbench::Collector collector(times);
			g_collector = &collector;
			const uint32_t total_times = warmup + times;
#else
			const uint32_t total_times = times;
#endif
			for (uint32_t i = 0; i < total_times; i++)
			{
				uint32_t actDate = 20220303;// strtoul("20220303", NULL, 10);
				uint32_t actTime = 100523 * 1000 + 500; //strToTime("10:05:23") * 1000 + 500;

				WTSContractInfo* contract = _bd_mgr->getContract("rb2205", "SHFE");
				if (contract == NULL)
					return;

				double x = rand();

				WTSCommodityInfo* pCommInfo = contract->getCommInfo();

				WTSTickData* tick = WTSTickData::create("rb2205");
				tick->setContractInfo(contract);

				WTSTickStruct& quote = tick->getTickStruct();
				wt_strcpy(quote.exchg, pCommInfo->getExchg());

				quote.action_date = actDate;
				quote.action_time = actTime;

				quote.price = x;
				quote.open = x;
				quote.high = x;
				quote.low = x;
				quote.total_volume = 0;
				quote.trading_date = 20220303;
				quote.settle_price = x;

				quote.open_interest = 0;

				quote.upper_limit = x;
				quote.lower_limit = x;

				quote.pre_close = x;
				quote.pre_settle = x;
				quote.pre_interest = 0;

				//委卖价格
				quote.ask_prices[0] = x;
				quote.ask_prices[1] = x;
				quote.ask_prices[2] = x;
				quote.ask_prices[3] = x;
				quote.ask_prices[4] = x;

				//委买价格
				quote.bid_prices[0] = x;
				quote.bid_prices[1] = x;
				quote.bid_prices[2] = x;
				quote.bid_prices[3] = x;
				quote.bid_prices[4] = x;

				//委卖量
				quote.ask_qty[0] = 0;
				quote.ask_qty[1] = 0;
				quote.ask_qty[2] = 0;
				quote.ask_qty[3] = 0;
				quote.ask_qty[4] = 0;

				//委买量
				quote.bid_qty[0] = 0;
				quote.bid_qty[1] = 0;
				quote.bid_qty[2] = 0;
				quote.bid_qty[3] = 0;
				quote.bid_qty[4] = 0;

#ifdef WT_HFT_BENCH
				collector.begin(i >= warmup);
#endif
				_parser_spi->handleQuote(tick, 0);
#ifdef WT_HFT_BENCH
				collector.end();
#endif
				tick->release();
			}
			auto total = ticker.nano_seconds();
#ifdef WT_HFT_BENCH
			collector.report("hft_benchmark_summary.csv");
			g_collector = nullptr;
			#ifdef WT_POOL_SINGLE_THREAD_FAST
			std::printf("pool_mode=single_thread_unlocked\n");
			#else
			std::printf("pool_mode=original_spinlock\n");
			#endif
			WTSLogger::warn("{} warmup and {} measured ticks completed in {:.0f} ns", warmup, times, total*1.0);
#else
			double t2t = total * 1.0 / times;
			WTSLogger::warn("{} ticks simulated in {:.0f} ns, HftEngine Innner Latency: {:.3f} ns", times, total*1.0, t2t);
#endif
		}

	public:
		virtual void registerSpi(IParserSpi* listener) override
		{
			_parser_spi = listener;
			_bd_mgr = listener->getBaseDataMgr();
		}

	private:
		IParserSpi*		_parser_spi;
		IBaseDataMgr*	_bd_mgr;
	};

	TestParser* theParser = NULL;

	class TestTrader : public ITraderApi
	{
	public:
		virtual bool init(WTSVariant*) override { return true; }

		virtual void registerSpi(ITraderSpi* listener) override
		{
			_trader_spi = listener;
		}

		virtual bool makeEntrustID(char* buffer, int length) override
		{
			wt_strcpy(buffer, "123456");
			return true;
		}

		virtual int orderInsert(WTSEntrust* eutrust) override
		{
#ifdef WT_HFT_BENCH
			if (g_collector) g_collector->api_enter();
			if (g_collector) g_collector->api_exit();
#endif
			return 0;
		}

	private:
		ITraderSpi*	_trader_spi;
	};

	class TestStrategy : public HftStrategy
	{
	public:
		TestStrategy(const char* id) : HftStrategy(id) {}

		/*
		*	执行单元名称
		*/
		virtual const char* getName() { return "TestStrategy"; }

		/*
		*	所属执行器工厂名称
		*/
		virtual const char* getFactName() { return "TestStrategyFact"; }


		virtual void on_init(IHftStraCtx* ctx) override
		{
			ctx->stra_sub_ticks("SHFE.rb.2205");
		}

		virtual void on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick)
		{
#ifdef WT_HFT_BENCH
			if (g_collector) g_collector->strategy_enter();
#endif
			//ctx->stra_sell("SHFE.rb.2205", 2300, 1, "", HFT_OrderFlag_Nor);
#ifdef WT_HFT_BENCH
			if (g_collector) g_collector->order_start();
#endif
			ctx->stra_buy("SHFE.rb.2205", 2300, 1, "", HFT_OrderFlag_Nor);
		}
	};


	HftLatencyTool::HftLatencyTool()
	{
	}


	HftLatencyTool::~HftLatencyTool()
	{
	}

	bool HftLatencyTool::init()
	{
		WTSLogger::init("logcfg.yaml");

		WTSVariant* _config = WTSCfgLoader::load_from_file("config.yaml");
		if (_config == NULL)
		{
			WTSLogger::log_raw(LL_ERROR, "Loading config file config.yaml failed");
			return false;
		}

		//基础数据文件
		WTSVariant* cfgBF = _config->get("basefiles");
		bool isUTF8 = cfgBF->getBoolean("utf-8");
		if (cfgBF->get("session"))
			_bd_mgr.loadSessions(cfgBF->getCString("session"));

		WTSVariant* cfgItem = cfgBF->get("commodity");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
			{
				_bd_mgr.loadCommodities(cfgItem->asCString());
			}
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
				{
					_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
				}
			}
		}

		cfgItem = cfgBF->get("contract");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
			{
				_bd_mgr.loadContracts(cfgItem->asCString());
			}
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
				{
					_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
				}
			}
		}

		if (cfgBF->get("hot"))
		{
			_hot_mgr.loadHots(cfgBF->getCString("hot"));
			WTSLogger::log_raw(LL_INFO, "Hot rules loades");
		}

		_act_mgr.init("actpolicy.yaml");

		_times = _config->getUInt32("times");
		WTSLogger::warn("{} ticks will be simulated", _times);

#ifdef WT_HFT_BENCH
		_warmup = _config->getUInt32("warmup");
		WTSLogger::warn("{} warmup ticks will run before measurement", _warmup);
#endif

		_core = _config->getUInt32("core");
		WTSLogger::warn("Testing thread will be bind to core {}", _core);

		initEngine(_config->get("env"));
		initModules();
		initStrategies();

		_config->release();
		return true;
	}

	bool HftLatencyTool::initStrategies()
	{
		HftStraContext* ctx = new HftStraContext(&_engine, "stra", false, 0);
		ctx->set_strategy(new TestStrategy("stra"));

		TraderAdapterPtr trader = _traders.getAdapter("trader");
		ctx->setTrader(trader.get());
		trader->addSink(ctx);

		_engine.addContext(HftContextPtr(ctx));

		return true;
	}

	bool HftLatencyTool::initEngine(WTSVariant* cfg)
	{
		WTSLogger::warn("Trading enviroment initialzied with engine: HFT");
		_engine.init(cfg, &_bd_mgr, &_dt_mgr, &_hot_mgr, NULL);
		_engine.set_date_time(20220303, 1005, 23500);
		_engine.set_trading_date(20220303);
		_engine.set_adapter_mgr(&_traders);

		return true;
	}


	bool HftLatencyTool::initModules()
	{
		{
			theParser = new TestParser();
			ParserAdapterPtr adapter(new ParserAdapter);
			adapter->initExt("parser", theParser, &_engine, &_bd_mgr, &_hot_mgr);
			_parsers.addAdapter("parser", adapter);
		}

		{
			TestTrader * tester = new TestTrader();
			TraderAdapterPtr adapter(new TraderAdapter());
			adapter->initExt("trader", tester, &_bd_mgr, &_act_mgr);
			_traders.addAdapter("trader", adapter);
		}

		return true;
	}

	void HftLatencyTool::run()
	{
		if (_core != 0)
		{
			if (!CpuHelper::bind_core(_core - 1))
			{
				WTSLogger::error("Binding to core {} failed", _core);
			}
		}

		try
		{
			_parsers.run();
			_traders.run();

			_engine.run();

#ifdef WT_HFT_BENCH
			theParser->run(_times, _warmup);
#else
			theParser->run(_times);
#endif
		}
		catch (...)
		{
		}
	}
}
