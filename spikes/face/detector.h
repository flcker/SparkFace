// Copyright 2026 Yakhyokhuja Valikhujaev
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "landmarks.h"
#include "preprocess.h"

#include <array>
#include <cstddef>
#include <vector>

constexpr std::size_t SF_FACE_ANCHOR_COUNT    = 896;
constexpr std::size_t SF_FACE_REGRESSOR_COUNT = 16;
constexpr std::size_t SF_FACE_KEYPOINT_COUNT  = 6;

struct SfPoint {
    float x;
    float y;
};

struct SfFaceRoi {
    float center_x;
    float center_y;
    float side;
    float angle_radians;
    float detection_score;
};

struct SfDetectorScratch {
    std::vector<std::array<float, 5 + SF_FACE_KEYPOINT_COUNT * 2>> candidates;
};

std::array<SfPoint, SF_FACE_ANCHOR_COUNT> sf_generate_face_anchors();

void sf_preprocess_detector(const SfImage& image, std::vector<float>& output);

bool sf_decode_face_roi(const float* regressors, std::size_t regressor_count, const float* logits,
                        std::size_t logit_count,
                        const std::array<SfPoint, SF_FACE_ANCHOR_COUNT>& anchors,
                        const SfImage& image, SfDetectorScratch& scratch, SfFaceRoi& roi);

void sf_preprocess_face_roi(const SfImage& image, const SfFaceRoi& roi, std::size_t input_size,
                            std::vector<float>& output);

void sf_map_face_landmarks(const float* values, std::size_t value_count, const SfFaceRoi& roi,
                           std::size_t input_size, const SfImage& image,
                           std::vector<SfLandmark>& landmarks);
