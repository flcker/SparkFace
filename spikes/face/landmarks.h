#pragma once

#include "preprocess.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

struct SfLandmark {
    float x;
    float y;
    float z;
};

enum class SfCoordinateMode {
    normalized_zero_one,
    normalized_minus_one_one,
    input_pixels,
};

std::vector<SfLandmark> sf_decode_landmarks(const float* values, std::size_t value_count,
                                            std::size_t landmark_count, std::size_t dimensions,
                                            SfCoordinateMode mode, std::uint32_t input_width,
                                            std::uint32_t input_height);

void sf_write_overlay_ppm(const SfImage& image, const std::vector<SfLandmark>& landmarks,
                          const std::filesystem::path& path);
