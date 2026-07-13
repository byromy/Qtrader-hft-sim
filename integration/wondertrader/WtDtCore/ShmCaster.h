#pragma once

#include "IDataCaster.h"
#include "../Share/BoostMappingFile.hpp"
#include "../Share/ShmRingProtocol.hpp"

namespace wtp { class WTSVariant; }
using namespace wtp;

class ShmCaster : public IDataCaster
{
public:
	ShmCaster();

	bool init(WTSVariant* cfg);
	virtual void broadcast(WTSTickData* curTick) override;
	virtual void broadcast(WTSOrdQueData* curOrdQue) override;
	virtual void broadcast(WTSOrdDtlData* curOrdDtl) override;
	virtual void broadcast(WTSTransData* curTrans) override;

private:
	void publish_item(const wtshm::DataItem& item);

	std::string _path;
	typedef std::shared_ptr<BoostMappingFile> MappedFilePtr;
	MappedFilePtr _mapfile;
	wtshm::LegacyQueue* _legacy_queue;
	wtshm::SequenceQueue* _sequence_queue;
	uint64_t _next_sequence;
	bool _sequence_protocol;
	bool _inited;
};
