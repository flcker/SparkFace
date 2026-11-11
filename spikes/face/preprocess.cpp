#include "preprocess.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::size_t checked_multiply(const std::size_t left, const std::size_t right,
                             const char* const description) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error(std::string("PPM ") + description + " is too large");
    }
    return left * right;
}

std::string read_ppm_token(std::istream& input, const char* const description) {
    for (;;) {
        const int next = input.peek();
        if (next == std::char_traits<char>::eof()) {
            throw std::runtime_error(std::string("PPM is missing ") + description);
        }

        const auto character = static_cast<unsigned char>(next);
        if (std::isspace(character) != 0) {
            input.get();
            continue;
        }

        if (character != '#') {
            break;
        }

        input.get();
        while (true) {
            const int comment_character = input.get();
            if (comment_character == std::char_traits<char>::eof() || comment_character == '\n' ||
                comment_character == '\r') {
                break;
            }
        }
    }

    std::string token;
    while (true) {
        const int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            break;
        }

        const auto character = static_cast<unsigned char>(next);
        if (std::isspace(character) != 0) {
            if (character == '\r' && input.peek() == '\n') {
                input.get();
            }
            break;
        }

        if (token.size() == 64) {
            throw std::runtime_error(std::string("PPM ") + description + " is too long");
        }
        token.push_back(static_cast<char>(character));
    }

    if (token.empty()) {
        throw std::runtime_error(std::string("PPM is missing ") + description);
    }
    return token;
}

std::uint32_t parse_ppm_uint(const std::string_view token, const char* const description) {
    std::uint32_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            throw std::runtime_error(std::string("invalid PPM ") + description);
        }

        const auto digit = static_cast<std::uint32_t>(character - '0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
            throw std::runtime_error(std::string("PPM ") + description + " is out of range");
        }
        value = value * 10 + digit;
    }
    return value;
}

double source_coordinate(const std::uint32_t destination_index,
                         const std::uint32_t destination_extent,
                         const std::uint32_t source_extent) noexcept {
    if (destination_extent == 1 || source_extent == 1) {
        return 0.0;
    }
    if (destination_index == destination_extent - 1) {
        return static_cast<double>(source_extent - 1);
    }
    return static_cast<double>(destination_index) * static_cast<double>(source_extent - 1) /
           static_cast<double>(destination_extent - 1);
}

} // namespace

SfImage sf_load_ppm(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open PPM file: " + path.string());
    }

    if (read_ppm_token(input, "magic") != "P6") {
        throw std::runtime_error("PPM magic must be P6");
    }

    const std::uint32_t width  = parse_ppm_uint(read_ppm_token(input, "width"), "width");
    const std::uint32_t height = parse_ppm_uint(read_ppm_token(input, "height"), "height");
    const std::uint32_t max_value =
        parse_ppm_uint(read_ppm_token(input, "maximum value"), "maximum value");
    if (width == 0 || height == 0) {
        throw std::runtime_error("PPM dimensions must be nonzero");
    }
    if (max_value != 255) {
        throw std::runtime_error("PPM maximum value must be 255");
    }

    const std::size_t pixel_count = checked_multiply(width, height, "pixel count");
    const std::size_t byte_count  = checked_multiply(pixel_count, 3, "byte count");
    if (byte_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::overflow_error("PPM byte count exceeds stream limits");
    }

    SfImage image{width, height, std::vector<std::uint8_t>(byte_count)};
    input.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(byte_count));
    if (input.gcount() != static_cast<std::streamsize>(byte_count)) {
        throw std::runtime_error("PPM pixel data byte count does not match dimensions");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("PPM contains trailing data after pixel bytes");
    }

    return image;
}

std::vector<float> sf_preprocess(const SfImage& image, const SfPreprocessSpec& spec) {
    if (image.width == 0 || image.height == 0) {
        throw std::invalid_argument("input image dimensions must be nonzero");
    }
    if (spec.width == 0 || spec.height == 0) {
        throw std::invalid_argument("preprocess dimensions must be nonzero");
    }
    if (spec.layout != SfTensorLayout::nchw && spec.layout != SfTensorLayout::nhwc) {
        throw std::invalid_argument("preprocess layout is invalid");
    }

    const std::size_t source_pixel_count =
        checked_multiply(image.width, image.height, "image pixel count");
    const std::size_t source_byte_count =
        checked_multiply(source_pixel_count, 3, "image byte count");
    if (image.rgb.size() != source_byte_count) {
        throw std::invalid_argument("input RGB buffer byte count does not match dimensions");
    }

    const std::size_t destination_pixel_count =
        checked_multiply(spec.width, spec.height, "output pixel count");
    const std::size_t destination_value_count =
        checked_multiply(destination_pixel_count, 3, "output value count");
    std::vector<float> output(destination_value_count);

    const auto resize = [&](const auto& write_pixel) {
        for (std::uint32_t destination_y = 0; destination_y < spec.height; ++destination_y) {
            const double source_y  = source_coordinate(destination_y, spec.height, image.height);
            const auto y0          = static_cast<std::uint32_t>(source_y);
            const auto y1          = std::min(y0 + 1, image.height - 1);
            const float y_fraction = static_cast<float>(source_y - static_cast<double>(y0));

            for (std::uint32_t destination_x = 0; destination_x < spec.width; ++destination_x) {
                const double source_x  = source_coordinate(destination_x, spec.width, image.width);
                const auto x0          = static_cast<std::uint32_t>(source_x);
                const auto x1          = std::min(x0 + 1, image.width - 1);
                const float x_fraction = static_cast<float>(source_x - static_cast<double>(x0));

                const std::size_t top_left_index =
                    (static_cast<std::size_t>(y0) * image.width + x0) * 3;
                const std::size_t top_right_index =
                    (static_cast<std::size_t>(y0) * image.width + x1) * 3;
                const std::size_t bottom_left_index =
                    (static_cast<std::size_t>(y1) * image.width + x0) * 3;
                const std::size_t bottom_right_index =
                    (static_cast<std::size_t>(y1) * image.width + x1) * 3;

                const auto interpolate = [&](const std::size_t channel) {
                    const float top_left = static_cast<float>(image.rgb[top_left_index + channel]);
                    const float top_right =
                        static_cast<float>(image.rgb[top_right_index + channel]);
                    const float bottom_left =
                        static_cast<float>(image.rgb[bottom_left_index + channel]);
                    const float bottom_right =
                        static_cast<float>(image.rgb[bottom_right_index + channel]);
                    const float top    = top_left + (top_right - top_left) * x_fraction;
                    const float bottom = bottom_left + (bottom_right - bottom_left) * x_fraction;
                    return top + (bottom - top) * y_fraction;
                };

                const std::size_t destination_index =
                    static_cast<std::size_t>(destination_y) * spec.width + destination_x;
                write_pixel(destination_index, interpolate(0), interpolate(1), interpolate(2));
            }
        }
    };

    const auto normalize = [&](const float value, const std::size_t channel) {
        return value * spec.scale[channel] + spec.bias[channel];
    };
    if (spec.layout == SfTensorLayout::nchw) {
        if (spec.bgr) {
            resize(
                [&](const std::size_t index, const float red, const float green, const float blue) {
                    output[index]                               = normalize(blue, 0);
                    output[destination_pixel_count + index]     = normalize(green, 1);
                    output[destination_pixel_count * 2 + index] = normalize(red, 2);
                });
        } else {
            resize(
                [&](const std::size_t index, const float red, const float green, const float blue) {
                    output[index]                               = normalize(red, 0);
                    output[destination_pixel_count + index]     = normalize(green, 1);
                    output[destination_pixel_count * 2 + index] = normalize(blue, 2);
                });
        }
    } else if (spec.bgr) {
        resize([&](const std::size_t index, const float red, const float green, const float blue) {
            const std::size_t output_index = index * 3;
            output[output_index]           = normalize(blue, 0);
            output[output_index + 1]       = normalize(green, 1);
            output[output_index + 2]       = normalize(red, 2);
        });
    } else {
        resize([&](const std::size_t index, const float red, const float green, const float blue) {
            const std::size_t output_index = index * 3;
            output[output_index]           = normalize(red, 0);
            output[output_index + 1]       = normalize(green, 1);
            output[output_index + 2]       = normalize(blue, 2);
        });
    }

    return output;
}
