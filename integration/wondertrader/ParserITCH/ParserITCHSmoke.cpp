#include "../Includes/IParserApi.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>

USING_NS_WTP;

namespace
{
	class SmokeSpi : public IParserSpi
	{
	public:
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleOrderDetail(WTSOrdDtlData* data) override
		{
			const auto& value = data->getOrdDtlStruct();
			++orders;
			if (std::strcmp(value.exchg, "PSX") != 0 || value.code[0] == '\0' ||
				value.trading_date == 0 || value.index == 0 || value.volume == 0)
				++errors;
		}
		void handleTransaction(WTSTransData* data) override
		{
			const auto& value = data->getTransStruct();
			++transactions;
			if (std::strcmp(value.exchg, "PSX") != 0 || value.code[0] == '\0' ||
				value.trading_date == 0 || value.index == 0 || value.volume == 0)
				++errors;
		}
		void handleParserLog(WTSLogLevel level, const char* message) override
		{
			if (level >= LL_ERROR)
			{
				++errors;
				std::fprintf(stderr, "%s\n", message);
			}
		}
		void handleEvent(WTSParserEvent event, int32_t code) override
		{
			if (event == WPE_Connect) ++connects;
			if (event == WPE_Login) ++logins;
			if (event == WPE_Close) ++closes;
			if (code != 0) ++errors;
		}
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }

		uint64_t orders = 0;
		uint64_t transactions = 0;
		uint64_t errors = 0;
		uint32_t connects = 0;
		uint32_t logins = 0;
		uint32_t closes = 0;
	};
}

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		std::fprintf(stderr, "usage: %s libParserITCH.so ITCH_FILE SYMBOL YYYYMMDD\n", argv[0]);
		return 64;
	}
	void* module = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (module == nullptr)
	{
		std::fprintf(stderr, "dlopen failed: %s\n", ::dlerror());
		return 1;
	}
	auto create = reinterpret_cast<FuncCreateParser>(::dlsym(module, "createParser"));
	auto remove = reinterpret_cast<FuncDeleteParser>(::dlsym(module, "deleteParser"));
	if (create == nullptr || remove == nullptr)
	{
		std::fprintf(stderr, "plugin entry points missing\n");
		::dlclose(module);
		return 1;
	}

	IParserApi* parser = create();
	SmokeSpi spi;
	WTSVariant* config = WTSVariant::createObject();
	config->append("path", argv[2]);
	config->append("symbol", argv[3]);
	config->append("date", static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)));
	config->append("gpsize", static_cast<uint32_t>(1000000));
	parser->registerSpi(&spi);
	const bool initialized = parser->init(config);
	config->release();
	const bool started = initialized && parser->connect();
	for (uint32_t wait = 0; started && parser->isConnected() && wait < 30000; ++wait)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const bool finished = started && !parser->isConnected();
	parser->release();
	remove(parser);
	::dlclose(module);

	std::printf("orders=%llu transactions=%llu connects=%u logins=%u closes=%u errors=%llu\n",
		static_cast<unsigned long long>(spi.orders),
		static_cast<unsigned long long>(spi.transactions), spi.connects, spi.logins,
		spi.closes, static_cast<unsigned long long>(spi.errors));
	return finished && spi.orders != 0 && spi.transactions != 0 &&
		spi.connects == 1 && spi.logins == 1 && spi.closes == 1 && spi.errors == 0 ? 0 : 3;
}
