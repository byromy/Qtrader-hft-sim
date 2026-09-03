#include "qtrader/WtsEventFingerprint.hpp"

#include <cassert>
#include <utility>

int main()
{
	wtp::WTSOrdDtlStruct first{};
	wtp::WTSOrdDtlStruct second{};
	first.index = 1;
	first.price = 100.25;
	first.volume = 10;
	second.index = 2;
	second.price = 100.50;
	second.volume = 20;

	qtrader::WtsEventFingerprint forward;
	forward.add(first);
	forward.add(second);

	qtrader::WtsEventFingerprint reversed;
	reversed.add(second);
	reversed.add(first);
	assert(forward.digest() != reversed.digest());

	auto changed = second;
	changed.action_time = 1;
	qtrader::WtsEventFingerprint field_changed;
	field_changed.add(first);
	field_changed.add(changed);
	assert(forward.digest() != field_changed.digest());

	qtrader::WtsEventFingerprint identical;
	identical.add(first);
	identical.add(second);
	assert(forward.digest() == identical.digest());
	assert(forward.count() == 2);
	return 0;
}
