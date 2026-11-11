#include "landmarks.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void sf_require_finite(float value, const char* name) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string("landmark ") + name + " must be finite");
    }
}

float sf_to_normalized(float value, SfCoordinateMode mode, std::uint32_t input_extent,
                       const char* axis_name) {
    sf_require_finite(value, axis_name);

    float normalized = 0.0F;
    switch (mode) {
    case SfCoordinateMode::normalized_zero_one:
        normalized = value;
        break;
    case SfCoordinateMode::normalized_minus_one_one:
        normalized = (value + 1.0F) * 0.5F;
        break;
    case SfCoordinateMode::input_pixels:
        if (input_extent == 0U) {
            throw std::invalid_argument("input dimensions must be non-zero");
        }
        normalized = input_extent == 1U ? value : value / static_cast<float>(input_extent - 1U);
        break;
    default:
        throw std::invalid_argument("unsupported landmark coordinate mode");
    }

    if (!std::isfinite(normalized) || normalized < 0.0F || normalized > 1.0F) {
        throw std::invalid_argument(std::string("landmark ") + axis_name +
                                    " is outside its expected coordinate range");
    }
    return normalized;
}

std::size_t sf_image_byte_count(const SfImage& image) {
    if (image.width == 0U || image.height == 0U) {
        throw std::invalid_argument("overlay image dimensions must be non-zero");
    }

    const std::size_t width         = static_cast<std::size_t>(image.width);
    const std::size_t height        = static_cast<std::size_t>(image.height);
    constexpr std::size_t kChannels = 3U;
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        width * height > std::numeric_limits<std::size_t>::max() / kChannels) {
        throw std::invalid_argument("overlay image dimensions overflow");
    }
    return width * height * kChannels;
}

void sf_draw_marker(std::vector<std::uint8_t>& overlay, const SfImage& image,
                    const SfLandmark& landmark) {
    sf_require_finite(landmark.x, "x");
    sf_require_finite(landmark.y, "y");
    sf_require_finite(landmark.z, "z");
    if (landmark.x < 0.0F || landmark.x > 1.0F || landmark.y < 0.0F || landmark.y > 1.0F) {
        throw std::invalid_argument("overlay landmark must be normalized to [0, 1]");
    }

    const std::int64_t center_x =
        std::llround(static_cast<double>(landmark.x) * static_cast<double>(image.width - 1U));
    const std::int64_t center_y =
        std::llround(static_cast<double>(landmark.y) * static_cast<double>(image.height - 1U));
    const std::size_t center_index =
        (static_cast<std::size_t>(center_y) * image.width + static_cast<std::size_t>(center_x)) *
        3U;
    const unsigned int center_luminance = static_cast<unsigned int>(image.rgb[center_index]) +
                                          static_cast<unsigned int>(image.rgb[center_index + 1U]) +
                                          static_cast<unsigned int>(image.rgb[center_index + 2U]);
    const std::uint8_t marker_value     = center_luminance >= 384U ? 0U : 255U;

    constexpr std::int64_t kMarkerRadius = 2;
    for (std::int64_t offset = -kMarkerRadius; offset <= kMarkerRadius; ++offset) {
        const std::int64_t horizontal_x = center_x + offset;
        if (horizontal_x >= 0 && horizontal_x < static_cast<std::int64_t>(image.width)) {
            const std::size_t pixel_index = (static_cast<std::size_t>(center_y) * image.width +
                                             static_cast<std::size_t>(horizontal_x)) *
                                            3U;
            overlay[pixel_index]          = marker_value;
            overlay[pixel_index + 1U]     = marker_value;
            overlay[pixel_index + 2U]     = marker_value;
        }

        const std::int64_t vertical_y = center_y + offset;
        if (vertical_y >= 0 && vertical_y < static_cast<std::int64_t>(image.height)) {
            const std::size_t pixel_index = (static_cast<std::size_t>(vertical_y) * image.width +
                                             static_cast<std::size_t>(center_x)) *
                                            3U;
            overlay[pixel_index]          = marker_value;
            overlay[pixel_index + 1U]     = marker_value;
            overlay[pixel_index + 2U]     = marker_value;
        }
    }
}

} // namespace

std::vector<SfLandmark> sf_decode_landmarks(const float* values, std::size_t value_count,
                                            std::size_t landmark_count, std::size_t dimensions,
                                            SfCoordinateMode mode, std::uint32_t input_width,
                                            std::uint32_t input_height) {
    if (landmark_count == 0U) {
        throw std::invalid_argument("landmark count must be non-zero");
    }
    if (dimensions != 2U && dimensions != 3U) {
        throw std::invalid_argument("landmark dimensions must be 2 or 3");
    }
    if (landmark_count > std::numeric_limits<std::size_t>::max() / dimensions) {
        throw std::invalid_argument("landmark output length overflows");
    }

    const std::size_t expected_value_count = landmark_count * dimensions;
    if (value_count != expected_value_count) {
        throw std::invalid_argument("landmark output length does not match shape");
    }
    if (values == nullptr) {
        throw std::invalid_argument("landmark output values must not be null");
    }
    if (mode == SfCoordinateMode::input_pixels && (input_width == 0U || input_height == 0U)) {
        throw std::invalid_argument("input dimensions must be non-zero");
    }

    std::vector<SfLandmark> landmarks;
    landmarks.reserve(landmark_count);
    for (std::size_t index = 0U; index < landmark_count; ++index) {
        const std::size_t value_index = index * dimensions;
        const float x = sf_to_normalized(values[value_index], mode, input_width, "x");
        const float y = sf_to_normalized(values[value_index + 1U], mode, input_height, "y");
        const float z = dimensions == 3U ? values[value_index + 2U] : 0.0F;
        sf_require_finite(z, "z");
        landmarks.push_back({x, y, z});
    }
    return landmarks;
}

void sf_write_overlay_ppm(const SfImage& image, const std::vector<SfLandmark>& landmarks,
                          const std::filesystem::path& path) {
    const std::size_t image_byte_count = sf_image_byte_count(image);
    if (image.rgb.size() != image_byte_count) {
        throw std::invalid_argument("overlay image RGB buffer length is invalid");
    }

    std::vector<std::uint8_t> overlay = image.rgb;
    for (const SfLandmark& landmark : landmarks) {
        sf_draw_marker(overlay, image, landmark);
    }

    if (overlay.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("overlay image is too large to write");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to open overlay PPM output");
    }
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    output.write(reinterpret_cast<const char*>(overlay.data()),
                 static_cast<std::streamsize>(overlay.size()));
    if (!output) {
        throw std::runtime_error("failed to write overlay PPM output");
    }
}
