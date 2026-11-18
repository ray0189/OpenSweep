#include <string>
#include <vector>

#include "framing.hpp"
#include "test_framework.hpp"

using namespace opensweep::framing;

namespace {
std::string frame(const std::string& payload) {
    std::string out;
    encode(payload, out, kMaxResponsePayload);
    return out;
}
std::string header(std::uint32_t n) {
    unsigned char h[4];
    write_header(n, h);
    return std::string(reinterpret_cast<const char*>(h), 4);
}
}  // namespace

TEST(framing_header_little_endian) {
    unsigned char h[4];
    write_header(0x01020304u, h);
    CHECK(h[0] == 0x04 && h[1] == 0x03 && h[2] == 0x02 && h[3] == 0x01);
    CHECK_EQ(read_header(h), 0x01020304u);
    std::string out;
    CHECK(encode("abc", out, 10));
    CHECK_EQ(out, std::string("\x03\x00\x00\x00" "abc", 7));
}

TEST(framing_encode_limits) {
    std::string out = "junk";
    CHECK(!encode("", out, 10));
    CHECK(out.empty());
    CHECK(encode(std::string(10, 'x'), out, 10));
    CHECK_EQ(out.size(), 14u);
    CHECK(!encode(std::string(11, 'x'), out, 10));
}

TEST(framing_single_and_multiple_frames) {
    Decoder d(kMaxRequestPayload);
    std::vector<std::string> frames;
    const std::string data = frame("{\"a\":1}") + frame("x") + frame("hello");
    CHECK(d.feed(data.data(), data.size(), frames) == FrameError::None);
    CHECK_EQ(frames.size(), 3u);
    CHECK_EQ(frames[0], std::string("{\"a\":1}"));
    CHECK_EQ(frames[1], std::string("x"));
    CHECK_EQ(frames[2], std::string("hello"));
    CHECK(!d.in_progress());
}

TEST(framing_split_chunks) {
    const std::string data = frame("first payload") + frame("second");
    // Feed every possible split into two chunks, and also byte-by-byte.
    for (std::size_t cut = 0; cut <= data.size(); ++cut) {
        Decoder d(kMaxRequestPayload);
        std::vector<std::string> frames;
        CHECK(d.feed(data.data(), cut, frames) == FrameError::None);
        CHECK(d.feed(data.data() + cut, data.size() - cut, frames) == FrameError::None);
        CHECK_EQ(frames.size(), 2u);
        if (frames.size() == 2) {
            CHECK_EQ(frames[0], std::string("first payload"));
            CHECK_EQ(frames[1], std::string("second"));
        }
    }
    Decoder d(kMaxRequestPayload);
    std::vector<std::string> frames;
    for (std::size_t i = 0; i < data.size(); ++i) {
        CHECK(d.feed(data.data() + i, 1, frames) == FrameError::None);
        if (i == 0) CHECK(d.in_progress());
    }
    CHECK_EQ(frames.size(), 2u);
    CHECK(!d.in_progress());
}

TEST(framing_exact_limit) {
    Decoder d(kMaxRequestPayload);
    std::vector<std::string> frames;
    std::string out;
    CHECK(encode(std::string(kMaxRequestPayload, 'a'), out, kMaxRequestPayload));
    CHECK(d.feed(out.data(), out.size(), frames) == FrameError::None);
    CHECK_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].size(), kMaxRequestPayload);
}

TEST(framing_over_limit) {
    Decoder d(kMaxRequestPayload);
    std::vector<std::string> frames;
    // Only the header is needed to reject: the server never buffers the oversized payload.
    const std::string h = header(static_cast<std::uint32_t>(kMaxRequestPayload + 1));
    CHECK(d.feed(h.data(), h.size(), frames) == FrameError::TooLarge);
    CHECK(frames.empty());
    // Sticky until reset.
    const std::string ok = frame("x");
    CHECK(d.feed(ok.data(), ok.size(), frames) == FrameError::TooLarge);
    CHECK(frames.empty());
    d.reset();
    CHECK(d.feed(ok.data(), ok.size(), frames) == FrameError::None);
    CHECK_EQ(frames.size(), 1u);

    Decoder d2(kMaxRequestPayload);
    const std::string huge = header(0xFFFFFFFFu);
    CHECK(d2.feed(huge.data(), huge.size(), frames) == FrameError::TooLarge);
}

TEST(framing_zero_length_is_bad_frame) {
    Decoder d(kMaxRequestPayload);
    std::vector<std::string> frames;
    const std::string data = frame("ok") + header(0) + frame("never");
    CHECK(d.feed(data.data(), data.size(), frames) == FrameError::BadFrame);
    CHECK_EQ(frames.size(), 1u);  // frames before the error are still delivered
    CHECK(d.error() == FrameError::BadFrame);
}

TEST(framing_partial_header_in_progress) {
    Decoder d(16);
    std::vector<std::string> frames;
    const std::string h = header(5);
    CHECK(!d.in_progress());
    CHECK(d.feed(h.data(), 2, frames) == FrameError::None);
    CHECK(d.in_progress());
    CHECK(d.feed(h.data() + 2, 2, frames) == FrameError::None);
    CHECK(d.in_progress());
    CHECK(d.feed("abcde", 5, frames) == FrameError::None);
    CHECK(!d.in_progress());
    CHECK_EQ(frames.size(), 1u);
}
