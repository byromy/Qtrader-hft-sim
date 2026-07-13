#include "ShmCaster.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../WTSTools/WTSLogger.h"

ShmCaster::ShmCaster()
	: _legacy_queue(nullptr), _sequence_queue(nullptr), _next_sequence(0),
	  _sequence_protocol(false), _inited(false)
{
}

bool ShmCaster::init(WTSVariant* cfg)
{
	if (cfg == nullptr || !cfg->getBoolean("active"))
		return false;

	_path = cfg->getCString("path");
	_sequence_protocol = cfg->get("protocol") != nullptr &&
		wt_stricmp(cfg->getCString("protocol"), "sequence") == 0;
	const std::size_t mapping_size = _sequence_protocol ?
		sizeof(wtshm::SequenceQueue) : sizeof(wtshm::LegacyQueue);

	BoostFile file;
	file.create_or_open_file(_path.c_str());
	file.truncate_file(mapping_size);
	file.close_file();

	_mapfile.reset(new BoostMappingFile);
	_mapfile->map(_path.c_str());

#ifdef _MSC_VER
	const uint32_t pid = _getpid();
#else
	const uint32_t pid = getpid();
#endif

	if (_sequence_protocol)
	{
		_sequence_queue = new(_mapfile->addr()) wtshm::SequenceQueue();
		_sequence_queue->header.pid = pid;
		_sequence_queue->header.epoch.store(wtshm::make_epoch(pid), std::memory_order_release);
		_next_sequence = 0;
	}
	else
	{
		_legacy_queue = new(_mapfile->addr()) wtshm::LegacyQueue();
		_legacy_queue->pid = pid;
	}

	_inited = true;
	WTSLogger::info("ShmCaster initialized @ {}, protocol={}", _path,
		_sequence_protocol ? "sequence" : "legacy");
	return true;
}

void ShmCaster::publish_item(const wtshm::DataItem& item)
{
	if (!_inited)
		return;

	if (_sequence_protocol)
	{
		wtshm::publish(*_sequence_queue, _next_sequence++, item);
		return;
	}

	const uint64_t sequence = _legacy_queue->writable++;
	std::memcpy(&_legacy_queue->items[sequence % _legacy_queue->capacity], &item, sizeof(item));
	_legacy_queue->readable = sequence;
}

void ShmCaster::broadcast(WTSTickData* curTick)
{
	if (curTick == nullptr)
		return;
	wtshm::DataItem item;
	item.type = static_cast<uint32_t>(wtshm::DataType::Tick);
	std::memcpy(&item.tick, &curTick->getTickStruct(), sizeof(WTSTickStruct));
	publish_item(item);
}

void ShmCaster::broadcast(WTSOrdQueData* curOrdQue)
{
	if (curOrdQue == nullptr)
		return;
	wtshm::DataItem item;
	item.type = static_cast<uint32_t>(wtshm::DataType::OrderQueue);
	std::memcpy(&item.queue, &curOrdQue->getOrdQueStruct(), sizeof(WTSOrdQueStruct));
	publish_item(item);
}

void ShmCaster::broadcast(WTSOrdDtlData* curOrdDtl)
{
	if (curOrdDtl == nullptr)
		return;
	wtshm::DataItem item;
	item.type = static_cast<uint32_t>(wtshm::DataType::OrderDetail);
	std::memcpy(&item.order, &curOrdDtl->getOrdDtlStruct(), sizeof(WTSOrdDtlStruct));
	publish_item(item);
}

void ShmCaster::broadcast(WTSTransData* curTrans)
{
	if (curTrans == nullptr)
		return;
	wtshm::DataItem item;
	item.type = static_cast<uint32_t>(wtshm::DataType::Transaction);
	std::memcpy(&item.trans, &curTrans->getTransStruct(), sizeof(WTSTransStruct));
	publish_item(item);
}
