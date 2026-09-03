#include "ParserITCH.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "qtrader/NasdaqItch50.hpp"
#include "qtrader/NasdaqItchFile.hpp"
#include "qtrader/NasdaqItchWtsAdapter.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace
{
	struct ReplayStopped final : std::exception {};

	std::string_view trimmed_symbol(const uint8_t* bytes)
	{
		size_t length = 8;
		while (length != 0 && bytes[length - 1] == ' ')
			--length;
		return {reinterpret_cast<const char*>(bytes), length};
	}

	class WonderTraderSink
	{
	public:
		explicit WonderTraderSink(IParserSpi* sink) : _sink(sink) {}

		void on_order_detail(const wtp::WTSOrdDtlStruct& value)
		{
			if (_sink == nullptr)
				return;
			auto copy = value;
			WTSOrdDtlData* data = WTSOrdDtlData::create(copy);
			_sink->handleOrderDetail(data);
			data->release();
		}

		void on_transaction(const wtp::WTSTransStruct& value)
		{
			if (_sink == nullptr)
				return;
			auto copy = value;
			WTSTransData* data = WTSTransData::create(copy);
			_sink->handleTransaction(data);
			data->release();
		}

	private:
		IParserSpi* _sink;
	};
}

extern "C"
{
	EXPORT_FLAG IParserApi* createParser() { return new ParserITCH(); }

	EXPORT_FLAG void deleteParser(IParserApi*& parser)
	{
		delete parser;
		parser = nullptr;
	}
}

ParserITCH::ParserITCH()
	: _trading_date(0), _gpsize(100000), _sink(nullptr), _stopped(true),
	  _connected(false)
{
}

ParserITCH::~ParserITCH()
{
	disconnect();
}

bool ParserITCH::init(WTSVariant* config)
{
	if (config == nullptr)
		return false;
	_path = config->getCString("path");
	_symbol = config->getCString("symbol");
	_trading_date = config->getUInt32("date");
	_gpsize = config->getUInt32("gpsize");
	if (_gpsize == 0)
		_gpsize = 100000;
	if (_path.empty() || _symbol.empty() || _trading_date == 0)
	{
		log(LL_ERROR, "[ParserITCH] path, symbol and date are required");
		return false;
	}
	return true;
}

void ParserITCH::release()
{
	disconnect();
}

bool ParserITCH::connect()
{
	if (_worker && _worker->joinable())
	{
		if (_connected.load(std::memory_order_acquire))
			return true;
		_worker->join();
		_worker.reset();
	}
	_stopped.store(false, std::memory_order_release);
	_connected.store(true, std::memory_order_release);
	_worker.reset(new std::thread([this]() { replay_loop(); }));
	return true;
}

bool ParserITCH::disconnect()
{
	_stopped.store(true, std::memory_order_release);
	if (_worker && _worker->joinable())
		_worker->join();
	_worker.reset();
	_connected.store(false, std::memory_order_release);
	return true;
}

bool ParserITCH::isConnected()
{
	return _connected.load(std::memory_order_acquire);
}

void ParserITCH::subscribe(const CodeSet& symbols)
{
	_subscriptions.insert(symbols.begin(), symbols.end());
}

void ParserITCH::unsubscribe(const CodeSet& symbols)
{
	for (const auto& symbol : symbols)
		_subscriptions.erase(symbol);
}

void ParserITCH::registerSpi(IParserSpi* listener)
{
	_sink = listener;
}

void ParserITCH::log(WTSLogLevel level, const std::string& message) const
{
	if (_sink)
		_sink->handleParserLog(level, message.c_str());
}

void ParserITCH::replay_loop()
{
	if (_sink)
	{
		_sink->handleEvent(WPE_Connect, 0);
		_sink->handleEvent(WPE_Login, 0);
	}

	try
	{
		std::unique_ptr<qtrader::itch50::PsxWtsAdapter> adapter;
		WonderTraderSink output(_sink);
		uint16_t locator = 0;
		uint64_t target_messages = 0;
		qtrader::itch50::scan_historical_file(
			_path.c_str(), [&](const uint8_t* message, size_t size)
			{
				if (_stopped.load(std::memory_order_acquire))
					throw ReplayStopped{};
				if (message[0] == 'R' &&
					size == qtrader::itch50::expected_message_size('R') &&
					trimmed_symbol(message + 11) == _symbol)
				{
					locator = qtrader::itch50::read_u16(message + 1);
					adapter.reset(new qtrader::itch50::PsxWtsAdapter(
						_trading_date, locator, _symbol));
				}
				if (adapter && size >= 11 &&
					qtrader::itch50::read_u16(message + 1) == locator)
				{
					adapter->on_message(message, size, output);
					++target_messages;
					if (target_messages % _gpsize == 0)
						log(LL_DEBUG, "[ParserITCH] replayed " +
							std::to_string(target_messages) + " target messages");
				}
			});

		if (!adapter)
			throw std::runtime_error("configured symbol was not found");
		const auto& stats = adapter->stats();
		if (stats.errors != 0 || adapter->active_orders() != 0)
			throw std::runtime_error("WTS conversion validation failed");
		log(LL_INFO, "[ParserITCH] replay complete, target=" +
			std::to_string(target_messages) + ", orders=" +
			std::to_string(stats.order_details) + ", transactions=" +
			std::to_string(stats.transactions));
	}
	catch (const ReplayStopped&)
	{
		log(LL_INFO, "[ParserITCH] replay stopped");
	}
	catch (const std::exception& error)
	{
		log(LL_ERROR, std::string("[ParserITCH] ") + error.what());
		if (_sink)
			_sink->handleEvent(WPE_Close, -1);
		_connected.store(false, std::memory_order_release);
		return;
	}

	if (_sink)
	{
		_sink->handleEvent(WPE_Logout, 0);
		_sink->handleEvent(WPE_Close, 0);
	}
	_connected.store(false, std::memory_order_release);
}
