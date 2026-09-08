#include "qtrader/SpscRing.hpp"
#include <cassert>
#include <thread>

int main()
{
    hftbench::SpscRing<unsigned, 2> small;
    unsigned value = 0;
    uint64_t dropped = 0;
    assert(!small.try_pop(value));
    assert(small.push(7, hftbench::FullPolicy::Reject) == hftbench::PushResult::Pushed);
    assert(small.push(8, hftbench::FullPolicy::Reject) == hftbench::PushResult::Full);
    assert(small.push(9, hftbench::FullPolicy::DropNewest, &dropped) == hftbench::PushResult::Dropped);
    assert(dropped == 1);
    assert(small.try_pop(value) && value == 7);
    assert(!small.try_pop(value));

    hftbench::SpscRing<unsigned, 256> ring;
    constexpr unsigned count = 100000;
    std::thread consumer([&] {
        for (unsigned expected = 0; expected < count; ++expected) {
            unsigned received = 0;
            while (!ring.try_pop(received)) std::this_thread::yield();
            assert(received == expected);
        }
    });
    for (unsigned i = 0; i < count; ++i)
        assert(ring.push(i, hftbench::FullPolicy::Spin) == hftbench::PushResult::Pushed);
    consumer.join();
    assert(!ring.try_pop(value));
}
