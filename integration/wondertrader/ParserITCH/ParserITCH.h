#pragma once

#include "../Includes/IParserApi.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

USING_NS_WTP;

class ParserITCH : public IParserApi
{
public:
	ParserITCH();
	~ParserITCH() override;

	bool init(WTSVariant* config) override;
	void release() override;
	bool connect() override;
	bool disconnect() override;
	bool isConnected() override;
	void subscribe(const CodeSet& symbols) override;
	void unsubscribe(const CodeSet& symbols) override;
	void registerSpi(IParserSpi* listener) override;

private:
	void replay_loop();
	void log(WTSLogLevel level, const std::string& message) const;

	std::string _path;
	std::string _symbol;
	uint32_t _trading_date;
	uint32_t _gpsize;
	IParserSpi* _sink;
	std::atomic<bool> _stopped;
	std::atomic<bool> _connected;
	std::unique_ptr<std::thread> _worker;
	CodeSet _subscriptions;
};
