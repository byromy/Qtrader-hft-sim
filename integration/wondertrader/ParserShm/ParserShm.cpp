#include "ParserShm.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/fmtlib.h"

template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel level, const char* format, const Args&... args)
{
	if (sink == nullptr)
		return;
	static thread_local char buffer[512] = {0};
	fmtutil::format_to(buffer, format, args...);
	sink->handleParserLog(level, buffer);
}

extern "C"
{
	EXPORT_FLAG IParserApi* createParser() { return new ParserShm(); }

	EXPORT_FLAG void deleteParser(IParserApi*& parser)
	{
		delete parser;
		parser = nullptr;
	}
}

ParserShm::ParserShm()
	: _legacy_queue(nullptr), _sequence_queue(nullptr), _gpsize(1000),
	  _check_span(0), _sequence_protocol(false), _start_at_latest(true),
	  _sink(nullptr), _stopped(false)
{
}

ParserShm::~ParserShm()
{
	disconnect();
}

bool ParserShm::init(WTSVariant* config)
{
	_path = config->getCString("path");
	_gpsize = config->getUInt32("gpsize");
	if (_gpsize == 0)
		_gpsize = 1000;
	_check_span = config->getUInt32("checkspan");
	_sequence_protocol = config->get("protocol") != nullptr &&
		wt_stricmp(config->getCString("protocol"), "sequence") == 0;
	_start_at_latest = config->get("start_at_latest") == nullptr ||
		config->getBoolean("start_at_latest");
	return true;
}

void ParserShm::release()
{
	disconnect();
}

bool ParserShm::connect()
{
	if (_thrd_parser)
		return true;
	_stopped.store(false, std::memory_order_release);
	_thrd_parser.reset(new StdThread([this]() { parser_loop(); }));
	return true;
}

void ParserShm::parser_loop()
{
	write_log(_sink, LL_INFO, "[ParserShm] loading {} ...", _path);
	while (!_stopped.load(std::memory_order_acquire) && !StdFile::exists(_path.c_str()))
	{
		write_log(_sink, LL_WARN, "[ParserShm] {} not present, retrying", _path);
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
	if (_stopped.load(std::memory_order_acquire))
		return;

	_mapfile.reset(new BoostMappingFile);
	_mapfile->map(_path.c_str());
	if (_sequence_protocol)
	{
		_sequence_queue = static_cast<wtshm::SequenceQueue*>(_mapfile->addr());
		if (!wtshm::valid(*_sequence_queue))
		{
			write_log(_sink, LL_ERROR, "[ParserShm] invalid sequence ring ABI");
			_sequence_queue = nullptr;
			return;
		}
	}
	else
	{
		_legacy_queue = static_cast<wtshm::LegacyQueue*>(_mapfile->addr());
	}

	if (_sink)
	{
		_sink->handleEvent(WPE_Connect, 0);
		_sink->handleEvent(WPE_Login, 0);
	}
	write_log(_sink, LL_INFO, "[ParserShm] receiving {}, protocol={}", _path,
		_sequence_protocol ? "sequence" : "legacy");

	if (_sequence_protocol)
		sequence_loop();
	else
		legacy_loop();
}

void ParserShm::idle_wait() const
{
	if (_check_span != 0)
		std::this_thread::sleep_for(std::chrono::microseconds(_check_span));
}

void ParserShm::sequence_loop()
{
	wtshm::SequenceReader reader;
	reader.attach(*_sequence_queue, _start_at_latest);
	uint64_t reported_drops = 0;
	uint64_t received = 0;
	wtshm::DataItem item;

	while (!_stopped.load(std::memory_order_acquire))
	{
		if (reader.restarted(*_sequence_queue))
		{
			write_log(_sink, LL_WARN, "[ParserShm] producer epoch changed, reattaching");
			reader.attach(*_sequence_queue, _start_at_latest);
		}

		if (!reader.try_read(*_sequence_queue, item))
		{
			idle_wait();
			continue;
		}

		dispatch(item);
		++received;
		if (reader.dropped() != reported_drops)
		{
			reported_drops = reader.dropped();
			write_log(_sink, LL_WARN, "[ParserShm] sequence gaps, dropped={} next={}",
				reported_drops, reader.next_sequence());
		}
		if (received % _gpsize == 0)
			write_log(_sink, LL_DEBUG, "[ParserShm] {} messages received, {} dropped", received, reported_drops);
	}
}

void ParserShm::legacy_loop()
{
	constexpr uint64_t no_data_seen = wtshm::kNoSequence - 1;
	uint32_t producer_pid = _legacy_queue->pid;
	uint64_t last = wtshm::kNoSequence;

	while (!_stopped.load(std::memory_order_acquire))
	{
		if (producer_pid != _legacy_queue->pid)
		{
			producer_pid = _legacy_queue->pid;
			last = wtshm::kNoSequence;
			write_log(_sink, LL_WARN, "[ParserShm] legacy producer restarted");
		}

		if (_legacy_queue->readable == wtshm::kNoSequence)
		{
			last = no_data_seen;
			idle_wait();
			continue;
		}
		if (last == wtshm::kNoSequence)
		{
			last = _legacy_queue->readable;
			idle_wait();
			continue;
		}
		if (last == no_data_seen)
			last = 0;
		else if (last >= _legacy_queue->readable)
		{
			idle_wait();
			continue;
		}
		else
			++last;

		wtshm::DataItem item;
		std::memcpy(&item, &_legacy_queue->items[last % _legacy_queue->capacity], sizeof(item));
		dispatch(item);
	}
}

void ParserShm::dispatch(const wtshm::DataItem& item)
{
	switch (static_cast<wtshm::DataType>(item.type))
	{
	case wtshm::DataType::Tick:
	{
		const char* full_code = fmtutil::format("{}.{}", item.tick.exchg, item.tick.code);
		if (_set_subs.find(full_code) == _set_subs.end())
			return;
		WTSTickData* data = WTSTickData::create(const_cast<WTSTickStruct&>(item.tick));
		if (_sink) _sink->handleQuote(data, 0);
		data->release();
		break;
	}
	case wtshm::DataType::OrderQueue:
	{
		const char* full_code = fmtutil::format("{}.{}", item.queue.exchg, item.queue.code);
		if (_set_subs.find(full_code) == _set_subs.end())
			return;
		WTSOrdQueData* data = WTSOrdQueData::create(const_cast<WTSOrdQueStruct&>(item.queue));
		if (_sink) _sink->handleOrderQueue(data);
		data->release();
		break;
	}
	case wtshm::DataType::OrderDetail:
	{
		const char* full_code = fmtutil::format("{}.{}", item.order.exchg, item.order.code);
		if (_set_subs.find(full_code) == _set_subs.end())
			return;
		WTSOrdDtlData* data = WTSOrdDtlData::create(const_cast<WTSOrdDtlStruct&>(item.order));
		if (_sink) _sink->handleOrderDetail(data);
		data->release();
		break;
	}
	case wtshm::DataType::Transaction:
	{
		const char* full_code = fmtutil::format("{}.{}", item.trans.exchg, item.trans.code);
		if (_set_subs.find(full_code) == _set_subs.end())
			return;
		WTSTransData* data = WTSTransData::create(const_cast<WTSTransStruct&>(item.trans));
		if (_sink) _sink->handleTransaction(data);
		data->release();
		break;
	}
	}
}

bool ParserShm::disconnect()
{
	_stopped.store(true, std::memory_order_release);
	if (_thrd_parser && _thrd_parser->joinable())
		_thrd_parser->join();
	_thrd_parser.reset();
	_sequence_queue = nullptr;
	_legacy_queue = nullptr;
	_mapfile.reset();
	return true;
}

bool ParserShm::isConnected()
{
	return _sequence_queue != nullptr || _legacy_queue != nullptr;
}

void ParserShm::subscribe(const CodeSet& symbols)
{
	_set_subs.insert(symbols.begin(), symbols.end());
}

void ParserShm::unsubscribe(const CodeSet& symbols)
{
	for (const auto& symbol : symbols)
		_set_subs.erase(symbol);
}

void ParserShm::registerSpi(IParserSpi* listener)
{
	const bool replaced = _sink != nullptr;
	_sink = listener;
	if (replaced && _sink)
		write_log(_sink, LL_WARN, "Listener is replaced");
}
