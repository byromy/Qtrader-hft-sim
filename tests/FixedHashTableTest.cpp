#include "qtrader/FixedHashTable.hpp"

#include <cassert>
#include <cstdint>

int main()
{
	qtrader::FixedHashTable<std::uint64_t> table(64);
	for (std::uint64_t key = 1; key <= 64; ++key)
		assert(table.insert(key, key * 10) ==
			qtrader::FixedHashInsertResult::Inserted);
	assert(table.size() == 64);
	assert(table.insert(1, 99) == qtrader::FixedHashInsertResult::Duplicate);
	assert(table.insert(65, 650) == qtrader::FixedHashInsertResult::Full);
	assert(table.insert(0, 0) == qtrader::FixedHashInsertResult::InvalidKey);

	for (std::uint64_t key = 2; key <= 64; key += 2)
		assert(table.erase(key));
	for (std::uint64_t key = 1; key <= 64; ++key)
	{
		const auto* value = table.find(key);
		if ((key & 1U) != 0)
			assert(value != nullptr && *value == key * 10);
		else
			assert(value == nullptr);
	}
	for (std::uint64_t key = 65; key <= 96; ++key)
		assert(table.insert(key, key * 10) ==
			qtrader::FixedHashInsertResult::Inserted);
	assert(table.size() == 64);
	return 0;
}
