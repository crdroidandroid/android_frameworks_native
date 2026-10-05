/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <inttypes.h>
#include <iosfwd>
#include <iostream>

#include <math/HashCombine.h>

namespace android {

struct BlurRegion {
    uint32_t blurRadius;
    float cornerRadiusTLX;
    float cornerRadiusTLY;
    float cornerRadiusTRX;
    float cornerRadiusTRY;
    float cornerRadiusBLX;
    float cornerRadiusBLY;
    float cornerRadiusBRX;
    float cornerRadiusBRY;
    float alpha;
    int left;
    int top;
    int right;
    int bottom;
    int32_t material = 0;

    inline bool operator==(const BlurRegion& other) const {
        return blurRadius == other.blurRadius && cornerRadiusTLX == other.cornerRadiusTLX &&
                cornerRadiusTLY == other.cornerRadiusTLY &&
                cornerRadiusTRX == other.cornerRadiusTRX &&
                cornerRadiusTRY == other.cornerRadiusTRY &&
                cornerRadiusBLX == other.cornerRadiusBLX &&
                cornerRadiusBLY == other.cornerRadiusBLY &&
                cornerRadiusBRX == other.cornerRadiusBRX &&
                cornerRadiusBRY == other.cornerRadiusBRY && alpha == other.alpha &&
                left == other.left && top == other.top && right == other.right &&
                bottom == other.bottom && material == other.material;
    }

    inline bool operator!=(const BlurRegion& other) const { return !(*this == other); }
};

namespace {
// A newline character followed by N*4 spaces.
static inline constexpr std::string IndentedNewline(uint8_t indent) {
    return "\n" + std::string(static_cast<size_t>(indent * 4), ' ');
}
} // namespace

static inline void PrintTo(const BlurRegion& blurRegion, ::std::ostream* os,
                           const uint8_t currentIndent = 0) {
    const std::string newline = IndentedNewline(currentIndent + 1);
    *os << "BlurRegion {";
    *os << newline << ".blurRadius = " << blurRegion.blurRadius;
    *os << newline << ".cornerRadiusTLX = " << blurRegion.cornerRadiusTLX;
    *os << newline << ".cornerRadiusTLY = " << blurRegion.cornerRadiusTLY;
    *os << newline << ".cornerRadiusTRX = " << blurRegion.cornerRadiusTRX;
    *os << newline << ".cornerRadiusTRY = " << blurRegion.cornerRadiusTRY;
    *os << newline << ".cornerRadiusBLX = " << blurRegion.cornerRadiusBLX;
    *os << newline << ".cornerRadiusBLY = " << blurRegion.cornerRadiusBLY;
    *os << newline << ".cornerRadiusBRX = " << blurRegion.cornerRadiusBRX;
    *os << newline << ".cornerRadiusBRY = " << blurRegion.cornerRadiusBRY;
    *os << newline << ".alpha = " << blurRegion.alpha;
    *os << newline << ".left = " << blurRegion.left;
    *os << newline << ".top = " << blurRegion.top;
    *os << newline << ".right = " << blurRegion.right;
    *os << newline << ".bottom = " << blurRegion.bottom;
    *os << IndentedNewline(currentIndent) << "}";
}

// copied from skia/src/core/SkBlurMask.cpp
inline float convertBlurUserRadiusToSigma(float radius) {
    return radius > 0 ? 0.57735f * radius + 0.5f : 0.0f;
}
// copied from skia/src/core/SkBlurEngine.h
inline float convertBlurSigmaToKernelRadius(float sigma) {
    return sigma <= 0.03f ? 0 : ceilf(3.f * sigma);
}

} // namespace android

namespace std {
template <>
struct hash<android::BlurRegion> {
    size_t operator()(const android::BlurRegion& region) const {
        return android::hashCombine(region.blurRadius, region.cornerRadiusTLX,
                                    region.cornerRadiusTLY, region.cornerRadiusTRX,
                                    region.cornerRadiusTRY, region.cornerRadiusBLX,
                                    region.cornerRadiusBLY, region.cornerRadiusBRX,
                                    region.cornerRadiusBRY, region.alpha, region.left, region.top,
                                    region.right, region.bottom);
    }
};
} // namespace std