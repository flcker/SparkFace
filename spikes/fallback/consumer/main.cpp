// SPDX-License-Identifier: Apache-2.0
#include "frame_transport.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string_view>
#include <string>
#include <vector>

namespace {

std::uint64_t embedded_sequence(const std::byte* frame) {
    if (std::memcmp(frame, "SFB2", 4) != 0) {
        throw std::runtime_error("Frame marker mismatch");
    }
    std::uint64_t value = 0;
    std::memcpy(&value, frame + 4, sizeof(value));
    std::uint32_t check = 0;
    std::memcpy(&check, frame + 12, sizeof(check));
    if (check != static_cast<std::uint32_t>(value ^ (value >> 32) ^ 0xB2F00D5AU)) {
        throw std::runtime_error("Frame checksum mismatch");
    }
    return value;
}

std::uint64_t parse_frames(int argc, char** argv) {
    std::uint64_t frames = 1000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--frames") {
            frames = std::stoull(argv[++i]);
        }
    }
    return frames;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::uint64_t target = parse_frames(argc, argv);
        sparkface::fallback::ConsumerRing ring;
        std::vector<std::byte> frame(sparkface::fallback::kNv12Bytes);
        std::uint64_t sequence = 0;
        std::uint64_t received = 0;
        std::uint64_t skipped = 0;
        std::uint64_t timeouts = 0;
        std::int64_t producer_qpc = 0;
        std::int64_t published_qpc = 0;

        while (received < target) {
            const std::uint64_t previous = sequence;
            if (!ring.read_latest(frame.data(), sequence, producer_qpc, published_qpc, 1000)) {
                ++timeouts;
                continue;
            }
            if (embedded_sequence(frame.data()) != sequence) {
                throw std::runtime_error("Embedded sequence does not match slot metadata");
            }
            if (previous != 0 && sequence > previous + 1) {
                skipped += sequence - previous - 1;
            }
            ++received;
        }

        std::cout << "consumer_frames=" << received << '\n'
                  << "consumer_last_sequence=" << sequence << '\n'
                  << "consumer_skipped_sequences=" << skipped << '\n'
                  << "consumer_timeouts=" << timeouts << '\n'
                  << "consumer_validation=pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "consumer_error=" << error.what() << '\n';
        return 1;
    }
}
