// Copyright 2026 Yakhyokhuja Valikhujaev
// SPDX-License-Identifier: Apache-2.0
#include "detector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

constexpr std::size_t kDetectorInputSize   = 128;
constexpr float kDetectionThreshold        = 0.5F;
constexpr float kNmsIouThreshold           = 0.3F;
constexpr float kRoiScale                  = 1.5F;
constexpr std::size_t kCandidateValueCount = 5 + SF_FACE_KEYPOINT_COUNT * 2;

void validate_image(const SfImage& image) {
    if (image.width == 0 || image.height == 0) {
        throw std::invalid_argument("image dimensions must be nonzero");
    }
    const std::size_t width  = image.width;
    const std::size_t height = image.height;
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        width * height > std::numeric_limits<std::size_t>::max() / 3 ||
        image.rgb.size() != width * height * 3) {
        throw std::invalid_argument("image RGB buffer length is invalid");
    }
}

std::array<float, 3> sample_rgb(const SfImage& image, const float x, const float y) {
    if (x < 0.0F || y < 0.0F || x > static_cast<float>(image.width - 1) ||
        y > static_cast<float>(image.height - 1)) {
        return {};
    }

    const auto x0          = static_cast<std::uint32_t>(x);
    const auto y0          = static_cast<std::uint32_t>(y);
    const std::uint32_t x1 = std::min(x0 + 1, image.width - 1);
    const std::uint32_t y1 = std::min(y0 + 1, image.height - 1);
    const float x_fraction = x - static_cast<float>(x0);
    const float y_fraction = y - static_cast<float>(y0);

    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        const auto value = [&](const std::uint32_t sample_x, const std::uint32_t sample_y) {
            return static_cast<float>(
                image.rgb[(static_cast<std::size_t>(sample_y) * image.width + sample_x) * 3 +
                          channel]);
        };
        const float top    = value(x0, y0) + (value(x1, y0) - value(x0, y0)) * x_fraction;
        const float bottom = value(x0, y1) + (value(x1, y1) - value(x0, y1)) * x_fraction;
        result[channel]    = top + (bottom - top) * y_fraction;
    }
    return result;
}

float sigmoid(const float value) {
    return static_cast<float>(1.0 / (1.0 + std::exp(-static_cast<double>(value))));
}

float candidate_iou(const std::array<float, kCandidateValueCount>& left,
                    const std::array<float, kCandidateValueCount>& right) {
    const float left_x1  = left[1] - left[3] * 0.5F;
    const float left_y1  = left[2] - left[4] * 0.5F;
    const float left_x2  = left[1] + left[3] * 0.5F;
    const float left_y2  = left[2] + left[4] * 0.5F;
    const float right_x1 = right[1] - right[3] * 0.5F;
    const float right_y1 = right[2] - right[4] * 0.5F;
    const float right_x2 = right[1] + right[3] * 0.5F;
    const float right_y2 = right[2] + right[4] * 0.5F;

    const float intersection_width =
        std::max(0.0F, std::min(left_x2, right_x2) - std::max(left_x1, right_x1));
    const float intersection_height =
        std::max(0.0F, std::min(left_y2, right_y2) - std::max(left_y1, right_y1));
    const float intersection = intersection_width * intersection_height;
    const float left_area = std::max(0.0F, left_x2 - left_x1) * std::max(0.0F, left_y2 - left_y1);
    const float right_area =
        std::max(0.0F, right_x2 - right_x1) * std::max(0.0F, right_y2 - right_y1);
    return intersection / std::max(left_area + right_area - intersection, 1.0e-9F);
}

} // namespace

std::array<SfPoint, SF_FACE_ANCHOR_COUNT> sf_generate_face_anchors() {
    constexpr std::array<std::size_t, 4> strides{8, 16, 16, 16};
    std::array<SfPoint, SF_FACE_ANCHOR_COUNT> anchors{};
    std::size_t anchor_index = 0;
    std::size_t layer        = 0;
    while (layer < strides.size()) {
        std::size_t next_layer = layer;
        while (next_layer < strides.size() && strides[next_layer] == strides[layer]) {
            ++next_layer;
        }
        const std::size_t repeats = 2 * (next_layer - layer);
        const std::size_t cells   = kDetectorInputSize / strides[layer];
        for (std::size_t y = 0; y < cells; ++y) {
            for (std::size_t x = 0; x < cells; ++x) {
                for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
                    if (anchor_index >= anchors.size()) {
                        throw std::logic_error("face anchor count overflow");
                    }
                    anchors[anchor_index++] = {
                        static_cast<float>(x + 0.5) / static_cast<float>(cells),
                        static_cast<float>(y + 0.5) / static_cast<float>(cells)};
                }
            }
        }
        layer = next_layer;
    }
    if (anchor_index != anchors.size()) {
        throw std::logic_error("face anchor count mismatch");
    }
    return anchors;
}

void sf_preprocess_detector(const SfImage& image, std::vector<float>& output) {
    validate_image(image);
    constexpr std::size_t pixel_count = kDetectorInputSize * kDetectorInputSize;
    if (output.size() != pixel_count * 3) {
        throw std::invalid_argument("detector input buffer length is invalid");
    }

    const float scale = static_cast<float>(kDetectorInputSize) /
                        static_cast<float>(std::max(image.width, image.height));
    const float pad_x =
        (static_cast<float>(kDetectorInputSize) - static_cast<float>(image.width) * scale) * 0.5F;
    const float pad_y =
        (static_cast<float>(kDetectorInputSize) - static_cast<float>(image.height) * scale) * 0.5F;
    for (std::size_t y = 0; y < kDetectorInputSize; ++y) {
        for (std::size_t x = 0; x < kDetectorInputSize; ++x) {
            const auto rgb          = sample_rgb(image, (static_cast<float>(x) - pad_x) / scale,
                                                 (static_cast<float>(y) - pad_y) / scale);
            const std::size_t pixel = y * kDetectorInputSize + x;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                output[channel * pixel_count + pixel] = (rgb[channel] - 127.5F) / 127.5F;
            }
        }
    }
}

bool sf_decode_face_roi(const float* const regressors, const std::size_t regressor_count,
                        const float* const logits, const std::size_t logit_count,
                        const std::array<SfPoint, SF_FACE_ANCHOR_COUNT>& anchors,
                        const SfImage& image, SfDetectorScratch& scratch, SfFaceRoi& roi) {
    validate_image(image);
    if (regressors == nullptr || logits == nullptr ||
        regressor_count != SF_FACE_ANCHOR_COUNT * SF_FACE_REGRESSOR_COUNT ||
        logit_count != SF_FACE_ANCHOR_COUNT) {
        throw std::invalid_argument("detector output buffer length is invalid");
    }

    scratch.candidates.clear();
    if (scratch.candidates.capacity() < SF_FACE_ANCHOR_COUNT) {
        scratch.candidates.reserve(SF_FACE_ANCHOR_COUNT);
    }
    for (std::size_t anchor_index = 0; anchor_index < anchors.size(); ++anchor_index) {
        const float score = sigmoid(logits[anchor_index]);
        if (score < kDetectionThreshold) {
            continue;
        }

        const float* const regressor = regressors + anchor_index * SF_FACE_REGRESSOR_COUNT;
        std::array<float, kCandidateValueCount> candidate{};
        candidate[0] = score;
        candidate[1] =
            regressor[0] / static_cast<float>(kDetectorInputSize) + anchors[anchor_index].x;
        candidate[2] =
            regressor[1] / static_cast<float>(kDetectorInputSize) + anchors[anchor_index].y;
        candidate[3] = regressor[2] / static_cast<float>(kDetectorInputSize);
        candidate[4] = regressor[3] / static_cast<float>(kDetectorInputSize);
        for (std::size_t keypoint = 0; keypoint < SF_FACE_KEYPOINT_COUNT; ++keypoint) {
            candidate[5 + keypoint * 2] =
                regressor[4 + keypoint * 2] / static_cast<float>(kDetectorInputSize) +
                anchors[anchor_index].x;
            candidate[6 + keypoint * 2] =
                regressor[5 + keypoint * 2] / static_cast<float>(kDetectorInputSize) +
                anchors[anchor_index].y;
        }
        scratch.candidates.push_back(candidate);
    }
    if (scratch.candidates.empty()) {
        return false;
    }

    std::sort(scratch.candidates.begin(), scratch.candidates.end(),
              [](const auto& left, const auto& right) { return left[0] > right[0]; });
    const auto& top = scratch.candidates.front();
    std::array<double, kCandidateValueCount> weighted{};
    double total_weight = 0.0;
    for (const auto& candidate : scratch.candidates) {
        if (candidate_iou(top, candidate) <= kNmsIouThreshold) {
            continue;
        }
        const double weight = candidate[0];
        total_weight += weight;
        for (std::size_t value = 1; value < candidate.size(); ++value) {
            weighted[value] += static_cast<double>(candidate[value]) * weight;
        }
    }
    if (total_weight <= 0.0) {
        throw std::runtime_error("detector weighted NMS produced no face");
    }

    std::array<float, kCandidateValueCount> blended = top;
    for (std::size_t value = 1; value < blended.size(); ++value) {
        blended[value] = static_cast<float>(weighted[value] / total_weight);
    }

    const float scale = static_cast<float>(kDetectorInputSize) /
                        static_cast<float>(std::max(image.width, image.height));
    const float pad_x =
        (static_cast<float>(kDetectorInputSize) - static_cast<float>(image.width) * scale) * 0.5F;
    const float pad_y =
        (static_cast<float>(kDetectorInputSize) - static_cast<float>(image.height) * scale) * 0.5F;
    const auto unletterbox_x = [&](const float normalized) {
        return (normalized * static_cast<float>(kDetectorInputSize) - pad_x) / scale;
    };
    const auto unletterbox_y = [&](const float normalized) {
        return (normalized * static_cast<float>(kDetectorInputSize) - pad_y) / scale;
    };

    const float box_width  = blended[3] * static_cast<float>(kDetectorInputSize) / scale;
    const float box_height = blended[4] * static_cast<float>(kDetectorInputSize) / scale;
    const float eye0_x     = unletterbox_x(blended[5]);
    const float eye0_y     = unletterbox_y(blended[6]);
    const float eye1_x     = unletterbox_x(blended[7]);
    const float eye1_y     = unletterbox_y(blended[8]);
    roi                    = {unletterbox_x(blended[1]), unletterbox_y(blended[2]),
                              kRoiScale * std::max(box_width, box_height),
                              std::atan2(eye1_y - eye0_y, eye1_x - eye0_x), blended[0]};
    return std::isfinite(roi.center_x) && std::isfinite(roi.center_y) && std::isfinite(roi.side) &&
           roi.side > 0.0F && std::isfinite(roi.angle_radians);
}

void sf_preprocess_face_roi(const SfImage& image, const SfFaceRoi& roi,
                            const std::size_t input_size, std::vector<float>& output) {
    validate_image(image);
    if (input_size == 0 || input_size > std::numeric_limits<std::size_t>::max() / input_size ||
        input_size * input_size > std::numeric_limits<std::size_t>::max() / 3 ||
        output.size() != input_size * input_size * 3 || !std::isfinite(roi.side) ||
        roi.side <= 0.0F) {
        throw std::invalid_argument("landmark ROI input is invalid");
    }

    const float cosine            = std::cos(roi.angle_radians);
    const float sine              = std::sin(roi.angle_radians);
    const float center            = static_cast<float>(input_size) * 0.5F;
    const float source_per_output = roi.side / static_cast<float>(input_size);
    const std::size_t pixel_count = input_size * input_size;
    for (std::size_t y = 0; y < input_size; ++y) {
        for (std::size_t x = 0; x < input_size; ++x) {
            const float relative_x  = (static_cast<float>(x) - center) * source_per_output;
            const float relative_y  = (static_cast<float>(y) - center) * source_per_output;
            const float source_x    = roi.center_x + cosine * relative_x - sine * relative_y;
            const float source_y    = roi.center_y + sine * relative_x + cosine * relative_y;
            const auto rgb          = sample_rgb(image, source_x, source_y);
            const std::size_t pixel = y * input_size + x;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                output[channel * pixel_count + pixel] = rgb[channel] / 255.0F;
            }
        }
    }
}

void sf_map_face_landmarks(const float* const values, const std::size_t value_count,
                           const SfFaceRoi& roi, const std::size_t input_size, const SfImage& image,
                           std::vector<SfLandmark>& landmarks) {
    validate_image(image);
    if (values == nullptr || input_size == 0 || value_count == 0 || value_count % 3 != 0 ||
        landmarks.size() != value_count / 3) {
        throw std::invalid_argument("landmark mapping buffer length is invalid");
    }

    const float cosine            = std::cos(roi.angle_radians);
    const float sine              = std::sin(roi.angle_radians);
    const float center            = static_cast<float>(input_size) * 0.5F;
    const float source_per_output = roi.side / static_cast<float>(input_size);
    const float width_scale       = image.width == 1 ? 1.0F : static_cast<float>(image.width - 1);
    const float height_scale      = image.height == 1 ? 1.0F : static_cast<float>(image.height - 1);
    for (std::size_t index = 0; index < landmarks.size(); ++index) {
        const float relative_x = (values[index * 3] - center) * source_per_output;
        const float relative_y = (values[index * 3 + 1] - center) * source_per_output;
        landmarks[index]       = {
            (roi.center_x + cosine * relative_x - sine * relative_y) / width_scale,
            (roi.center_y + sine * relative_x + cosine * relative_y) / height_scale,
            values[index * 3 + 2] * source_per_output,
        };
        if (!std::isfinite(landmarks[index].x) || !std::isfinite(landmarks[index].y) ||
            !std::isfinite(landmarks[index].z)) {
            throw std::runtime_error("mapped landmark is not finite");
        }
    }
}
