/*
 * Copyright 2021 The Android Open Source Project
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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "GaussianBlurFilter.h"
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkImageFilters.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include "include/gpu/GpuTypes.h" // from Skia
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

// This constant approximates the scaling done in the software path's
// "high quality" mode, in SkBlurMask::Blur() (1 / sqrt(3)).
static const float BLUR_SIGMA_SCALE = 0.1;
static const float GLASS_BLUR_SCALE = 2.0f;

GaussianBlurFilter::GaussianBlurFilter() : BlurFilter(/* maxCrossFadeRadius= */ 0.0f) {}

sk_sp<SkImage> GaussianBlurFilter::generate(GrRecordingContext* context, const uint32_t blurRadius,
                                            const sk_sp<SkImage> input, const SkRect& blurRect)
    const {
    // Calculate the scaled dimensions once
    const float scaledWidth = std::ceil(blurRect.width() * kInputScale);
    const float scaledHeight = std::ceil(blurRect.height() * kInputScale);

    // Create a surface with the scaled dimensions
    SkImageInfo scaledInfo = input->imageInfo().makeWH(scaledWidth, scaledHeight);
    sk_sp<SkSurface> surface = SkSurface::MakeRenderTarget(context,
                                                           skgpu::Budgeted::kNo, scaledInfo);

    // Prepare the blur filter parameters
    const float sigmaScale = blurRadius * kInputScale * BLUR_SIGMA_SCALE;

    // Set up the paint with the blur filter and mirror-like glass effect
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);

    // Create the blur filter with mirror tile mode
    sk_sp<SkImageFilter> blurFilter =
        SkImageFilters::Blur(sigmaScale, sigmaScale, SkTileMode::kMirror, nullptr);

    // Create the mirror-like glass effect with mirror tile mode
    sk_sp<SkImageFilter> glassEffect = SkImageFilters::Blur(
        blurRadius * kInputScale * GLASS_BLUR_SCALE, blurRadius * kInputScale * GLASS_BLUR_SCALE,
        SkTileMode::kMirror, nullptr);

    // Apply the mirror-like glass effect on top of the blur filter
    sk_sp<SkImageFilter> finalFilter = SkImageFilters::Compose(blurFilter, glassEffect);

    // Attach the final filter to the paint
    paint.setImageFilter(finalFilter);

    // Draw the image onto the surface with the filter applied
    surface->getCanvas()->drawImageRect(
        input, blurRect, SkRect::MakeWH(scaledWidth, scaledHeight),
        SkSamplingOptions{SkFilterMode::kLinear, SkMipmapMode::kNone}, &paint,
        SkCanvas::SrcRectConstraint::kFast_SrcRectConstraint);

    return surface->makeImageSnapshot();
}

} // namespace skia
} // namespace renderengine
} // namespace android
