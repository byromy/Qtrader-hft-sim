#include "../Share/ShmRingProtocol.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#ifndef _MSC_VER
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	bool expect(bool condition, const char* message)
	{
		if (!condition)
			std::fprintf(stderr, "FAILED: %s\n", message);
		return condition;
	}
}

int main()
{
	std::unique_ptr<wtshm::SequenceQueue> queue(new wtshm::SequenceQueue());
	queue->header.epoch.store(1, std::memory_order_release);
	bool ok = expect(wtshm::valid(*queue), "new queue ABI must be valid");

	wtshm::SequenceReader ordered_reader;
	ordered_reader.attach(*queue, false);
	for (uint64_t sequence = 0; sequence < 1000; ++sequence)
	{
		wtshm::DataItem item;
		item.type = static_cast<uint32_t>(wtshm::DataType::Tick);
		item.tick.action_time = static_cast<uint32_t>(sequence);
		wtshm::publish(*queue, sequence, item);
		wtshm::DataItem output;
		ok &= expect(ordered_reader.try_read(*queue, output), "published item must be readable");
		ok &= expect(output.tick.action_time == sequence, "payload must match its sequence");
	}
	ok &= expect(ordered_reader.dropped() == 0, "ordered reader must not drop data");
	wtshm::SequenceReader second_reader;
	second_reader.attach(*queue, false);
	for (uint64_t expected = 0; expected < 1000; ++expected)
	{
		wtshm::DataItem output;
		ok &= expect(second_reader.try_read(*queue, output), "second broadcast reader must see every item");
		ok &= expect(output.tick.action_time == expected, "second reader payload must preserve order");
	}
	ok &= expect(second_reader.dropped() == 0, "second broadcast reader must not compete with first reader");

	queue.reset(new wtshm::SequenceQueue());
	queue->header.epoch.store(2, std::memory_order_release);
	wtshm::SequenceReader slow_reader;
	slow_reader.attach(*queue, false);
	const uint64_t overrun_count = wtshm::kCapacity + 37;
	for (uint64_t sequence = 0; sequence < overrun_count; ++sequence)
	{
		wtshm::DataItem item;
		item.tick.action_time = static_cast<uint32_t>(sequence);
		wtshm::publish(*queue, sequence, item);
	}
	wtshm::DataItem output;
	ok &= expect(slow_reader.try_read(*queue, output), "slow reader must resume at oldest available item");
	ok &= expect(slow_reader.dropped() == 37, "overwritten item count must be exact");
	ok &= expect(output.tick.action_time == 37, "reader must resume at the oldest retained sequence");

	queue->header.epoch.store(3, std::memory_order_release);
	ok &= expect(slow_reader.restarted(*queue), "epoch change must identify producer restart");

#ifndef _MSC_VER
	void* shared = ::mmap(nullptr, sizeof(wtshm::SequenceQueue), PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ok &= expect(shared != MAP_FAILED, "process-shared mapping must succeed");
	if (shared != MAP_FAILED)
	{
		auto* process_queue = new(shared) wtshm::SequenceQueue();
		process_queue->header.epoch.store(4, std::memory_order_release);
		const pid_t child = ::fork();
		ok &= expect(child >= 0, "fork must succeed");
		if (child == 0)
		{
			wtshm::SequenceReader reader;
			reader.attach(*process_queue, false);
			wtshm::DataItem item;
			for (uint64_t expected = 0; expected < 4000;)
			{
				if (!reader.try_read(*process_queue, item)) continue;
				if (item.tick.action_time != expected) ::_exit(2);
				++expected;
			}
			::_exit(reader.dropped() == 0 ? 0 : 3);
		}
		for (uint64_t sequence = 0; child > 0 && sequence < 4000; ++sequence)
		{
			wtshm::DataItem item;
			item.tick.action_time = static_cast<uint32_t>(sequence);
			wtshm::publish(*process_queue, sequence, item);
		}
		if (child > 0)
		{
			int status = 0;
			::waitpid(child, &status, 0);
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"forked reader must observe all released payloads");
		}
		::munmap(shared, sizeof(wtshm::SequenceQueue));
	}
#endif

	std::printf("ShmRingProtocol: ordered=1000 broadcast_readers=2 overrun_dropped=%llu restart_detected=1 interprocess=ok\n",
		static_cast<unsigned long long>(slow_reader.dropped()));
	return ok ? 0 : 1;
}
