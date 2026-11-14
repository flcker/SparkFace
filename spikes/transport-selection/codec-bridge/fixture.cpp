// SPDX-License-Identifier: Apache-2.0

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t Width = 1920;
constexpr std::uint32_t Height = 1080;
constexpr std::size_t FrameBytes =
    static_cast<std::size_t>(Width) * Height * 3 / 2;
constexpr std::uint16_t PayloadMagic = 0xD311U;

void encode_word(std::byte* luma, std::uint32_t firstBit,
                 std::uint32_t bitCount, std::uint64_t value) {
    for (std::uint32_t bit = 0; bit < bitCount; ++bit) {
        const auto block = firstBit + bit;
        const std::byte encoded =
            ((value >> bit) & 1U) != 0 ? std::byte{235} : std::byte{16};
        std::fill_n(luma + block * 8, 8, encoded);
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("usage: transport_codec_fixture OUTPUT");
        }
        std::vector<std::byte> frame(FrameBytes, std::byte{16});
        std::fill(frame.begin() + static_cast<std::ptrdiff_t>(Width) * Height,
                  frame.end(), std::byte{128});
        encode_word(frame.data(), 0, 16, PayloadMagic);
        encode_word(frame.data(), 16, 64, 1);
        encode_word(frame.data(), 80, 64, 1);
        encode_word(frame.data(), 144, 32, 1);
        std::ofstream output(std::filesystem::path(argv[1]),
                             std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(frame.data()),
                     static_cast<std::streamsize>(frame.size()));
        if (!output) {
            throw std::runtime_error("failed to write fixture");
        }
        std::cout << "codec_fixture_bytes=" << frame.size() << '\n'
                  << "CODEC_FIXTURE_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CODEC_FIXTURE_ERROR: " << error.what() << '\n';
        return 1;
    }
}
