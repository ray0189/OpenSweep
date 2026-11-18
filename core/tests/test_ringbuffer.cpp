#include <string>
#include <vector>

#include "ringbuffer.hpp"
#include "test_framework.hpp"

using opensweep::RingBuffer;

TEST(ringbuffer_basic) {
    RingBuffer<int> rb(3);
    CHECK(rb.empty());
    CHECK_EQ(rb.capacity(), 3u);
    CHECK(!rb.push(1));
    CHECK(!rb.push(2));
    CHECK_EQ(rb.size(), 2u);
    CHECK_EQ(rb.front(), 1);
    CHECK_EQ(rb.back(), 2);
    CHECK(!rb.push(3));
    CHECK(rb.full());
    CHECK(rb.push(4));  // overwrites 1
    CHECK_EQ(rb.size(), 3u);
    CHECK((rb.to_vector() == std::vector<int>{2, 3, 4}));
    CHECK_EQ(rb[0], 2);
    CHECK_EQ(rb[2], 4);
    CHECK_THROWS(rb.at(3));
}

TEST(ringbuffer_wraparound_many) {
    RingBuffer<int> rb(5);
    for (int i = 0; i < 1000; ++i) rb.push(i);
    CHECK((rb.to_vector() == std::vector<int>{995, 996, 997, 998, 999}));
    CHECK_EQ(rb.pop_front(), 995);
    CHECK_EQ(rb.size(), 4u);
    rb.push(1000);
    CHECK((rb.to_vector() == std::vector<int>{996, 997, 998, 999, 1000}));
}

TEST(ringbuffer_pop_and_clear) {
    RingBuffer<std::string> rb(2);
    CHECK_THROWS(rb.pop_front());
    CHECK_THROWS(rb.back());
    rb.push("a");
    rb.push("b");
    CHECK_EQ(rb.pop_front(), std::string("a"));
    CHECK_EQ(rb.pop_front(), std::string("b"));
    CHECK(rb.empty());
    rb.push("c");
    rb.clear();
    CHECK(rb.empty());
    CHECK(rb.to_vector().empty());
    rb.push("d");
    CHECK_EQ(rb.front(), std::string("d"));
}

TEST(ringbuffer_zero_capacity_rejected) { CHECK_THROWS(RingBuffer<int>(0)); }

TEST(ringbuffer_capacity_one) {
    RingBuffer<int> rb(1);
    rb.push(1);
    CHECK(rb.push(2));
    CHECK_EQ(rb.front(), 2);
    CHECK_EQ(rb.size(), 1u);
}
