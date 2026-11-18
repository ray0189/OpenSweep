// Length-prefixed framing for the OpenSweep Core IPC protocol v1.
// Every message = 4-byte little-endian unsigned length N, then N bytes of payload.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opensweep::framing {

inline constexpr std::size_t kHeaderSize = 4;
inline constexpr std::size_t kMaxRequestPayload = 64 * 1024;     // 65 536 bytes
inline constexpr std::size_t kMaxResponsePayload = 1024 * 1024;  // 1 048 576 bytes

enum class FrameError {
    None,
    BadFrame,  // zero-length frame
    TooLarge,  // declared length exceeds the decoder's limit
};

// Encodes `payload` into `out` (replacing its contents). Returns false (and leaves `out`
// empty) if the payload is empty or larger than `max_payload`.
bool encode(std::string_view payload, std::string& out, std::size_t max_payload);

// Writes the 4-byte little-endian header for length n.
void write_header(std::uint32_t n, unsigned char* dst) noexcept;
std::uint32_t read_header(const unsigned char* src) noexcept;

// Incremental decoder. Bytes may arrive in arbitrary chunks. Memory use is bounded by
// kHeaderSize + max_payload per decoder. Errors are sticky: once an error is reported the
// decoder consumes nothing more until reset().
class Decoder {
public:
    explicit Decoder(std::size_t max_payload) noexcept : max_payload_(max_payload) {}

    // Consumes `len` bytes and appends each completed payload to `frames`. Stops at the first
    // error. Returns the (possibly sticky) error state.
    FrameError feed(const void* data, std::size_t len, std::vector<std::string>& frames);

    FrameError error() const noexcept { return error_; }
    // True while a frame has been started (at least one byte received) but is incomplete.
    bool in_progress() const noexcept { return header_have_ > 0 || expected_ > 0; }
    std::size_t max_payload() const noexcept { return max_payload_; }
    void reset() noexcept;

private:
    std::size_t max_payload_;
    std::array<unsigned char, kHeaderSize> header_{};
    std::size_t header_have_ = 0;  // bytes of the current header received
    std::size_t expected_ = 0;     // payload length of the current frame (0 = reading header)
    std::string payload_;
    FrameError error_ = FrameError::None;
};

}  // namespace opensweep::framing
