#include "framing.hpp"

#include <algorithm>

namespace opensweep::framing {

void write_header(std::uint32_t n, unsigned char* dst) noexcept {
    dst[0] = static_cast<unsigned char>(n & 0xFFu);
    dst[1] = static_cast<unsigned char>((n >> 8) & 0xFFu);
    dst[2] = static_cast<unsigned char>((n >> 16) & 0xFFu);
    dst[3] = static_cast<unsigned char>((n >> 24) & 0xFFu);
}

std::uint32_t read_header(const unsigned char* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[3]) << 24);
}

bool encode(std::string_view payload, std::string& out, std::size_t max_payload) {
    out.clear();
    if (payload.empty() || payload.size() > max_payload || payload.size() > 0xFFFFFFFFu) return false;
    unsigned char hdr[kHeaderSize];
    write_header(static_cast<std::uint32_t>(payload.size()), hdr);
    out.reserve(kHeaderSize + payload.size());
    out.append(reinterpret_cast<const char*>(hdr), kHeaderSize);
    out.append(payload.data(), payload.size());
    return true;
}

void Decoder::reset() noexcept {
    header_have_ = 0;
    expected_ = 0;
    payload_.clear();
    payload_.shrink_to_fit();
    error_ = FrameError::None;
}

FrameError Decoder::feed(const void* data, std::size_t len, std::vector<std::string>& frames) {
    if (error_ != FrameError::None) return error_;
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t i = 0;
    while (i < len) {
        if (expected_ == 0) {
            // Reading the header.
            const std::size_t take = std::min(kHeaderSize - header_have_, len - i);
            std::copy_n(p + i, take, header_.begin() + static_cast<std::ptrdiff_t>(header_have_));
            header_have_ += take;
            i += take;
            if (header_have_ < kHeaderSize) break;
            const std::uint32_t n = read_header(header_.data());
            if (n == 0) {
                error_ = FrameError::BadFrame;
                return error_;
            }
            if (n > max_payload_) {
                error_ = FrameError::TooLarge;
                return error_;
            }
            expected_ = n;
            payload_.clear();
            payload_.reserve(n);
            continue;
        }
        const std::size_t take = std::min(expected_ - payload_.size(), len - i);
        payload_.append(reinterpret_cast<const char*>(p + i), take);
        i += take;
        if (payload_.size() == expected_) {
            frames.push_back(std::move(payload_));
            payload_ = std::string();
            expected_ = 0;
            header_have_ = 0;
        }
    }
    return error_;
}

}  // namespace opensweep::framing
