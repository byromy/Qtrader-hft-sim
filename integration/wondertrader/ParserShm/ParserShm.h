#pragma once

#include "../Includes/IParserApi.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"
#include "../Share/ShmRingProtocol.hpp"

#include <atomic>

USING_NS_WTP;

class ParserShm : public IParserApi
{
public:
	ParserShm();
	~ParserShm();

	virtual bool init(WTSVariant* config) override;
	virtual void release() override;
	virtual bool connect() override;
	virtual bool disconnect() override;
	virtual bool isConnected() override;
	virtual void subscribe(const CodeSet& vecSymbols) override;
	virtual void unsubscribe(const CodeSet& vecSymbols) override;
	virtual void registerSpi(IParserSpi* listener) override;

private:
	void parser_loop();
	void sequence_loop();
	void legacy_loop();
	void dispatch(const wtshm::DataItem& item);
	void idle_wait() const;

	std::string _path;
	typedef std::shared_ptr<BoostMappingFile> MappedFilePtr;
	MappedFilePtr _mapfile;
	wtshm::LegacyQueue* _legacy_queue;
	wtshm::SequenceQueue* _sequence_queue;
	uint32_t _gpsize;
	uint32_t _check_span;
	bool _sequence_protocol;
	bool _start_at_latest;
	IParserSpi* _sink;
	std::atomic<bool> _stopped;
	CodeSet _set_subs;
	StdThreadPtr _thrd_parser;
};
