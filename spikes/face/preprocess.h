#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

struct SfImage {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> rgb;
};

enum class SfTensorLayout {
    nchw,
    nhwc,
};

struct SfPreprocessSpec {
    std::uint32_t width;
    std::uint32_t height;
    SfTensorLayout layout;
    bool bgr;
    std::array<float, 3> scale;
    std::array<float, 3> bias;
};

SfImage sf_load_ppm(const std::filesystem::path& path);
std::vector<float> sf_preprocess(const SfImage& image, const SfPreprocessSpec& spec);
