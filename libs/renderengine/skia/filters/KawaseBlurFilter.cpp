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

#include "KawaseBlurFilter.h"
#include <SkCanvas.h>
#include <SkPaint.h>
#include "SkImageFilters.h"
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

KawaseBlurFilter::KawaseBlurFilter() : BlurFilter() {
    SkString blurString(R"(
        uniform shader child;
        uniform float in_blurOffset;

        half4 main(float2 xy) {
            half4 c = child.eval(xy);
            c += child.eval(xy + float2(+in_blurOffset, +in_blurOffset));
            c += child.eval(xy + float2(+in_blurOffset, -in_blurOffset));
            c += child.eval(xy + float2(-in_blurOffset, -in_blurOffset));
            c += child.eval(xy + float2(-in_blurOffset, +in_blurOffset));
            return half4(c.rgb * 0.2, 1.0);
        }
    )");

    auto [blurEffect, error] = SkRuntimeEffect::MakeForShader(blurString);
    if (!blurEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", error.c_str());
    }
    mBlurEffect = std::move(blurEffect);
}

sk_sp<SkImage> KawaseBlurFilter::generate(GrRecordingContext* context, const uint32_t blurRadius,
                                          const sk_sp<SkImage> input, const SkRect& blurRect)
    const {
    float tmpRadius = (float)blurRadius / 6.0f;
    float numberOfPasses = std::min(kMaxPasses, (uint32_t)ceil(tmpRadius));
    float radiusByPasses = tmpRadius / (float)numberOfPasses;

    SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
    blurMatrix.postScale(kInputScale, kInputScale);

    SkSamplingOptions linear(SkFilterMode::kLinear, SkMipmapMode::kNone);
    SkRuntimeShaderBuilder blurBuilder(mBlurEffect);
    blurBuilder.child("child") =
            input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linear, blurMatrix);
    blurBuilder.uniform("in_blurOffset") = radiusByPasses * kInputScale;

    SkImageInfo scaledInfo = input->imageInfo().makeWH(std::ceil(blurRect.width() * kInputScale),
                                                       std::ceil(blurRect.height() * kInputScale));

    sk_sp<SkImage> tmpBlur = blurBuilder.makeImage(context, nullptr, scaledInfo, false);
    if (!tmpBlur) {
        return nullptr;
    }

    for (auto i = 1; i < numberOfPasses; i++) {
        blurBuilder.child("child") =
                tmpBlur->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linear);
        blurBuilder.uniform("in_blurOffset") = (float) i * radiusByPasses * kInputScale;
        sk_sp<SkImage> nextBlur = blurBuilder.makeImage(context, nullptr, scaledInfo, false);
        if (!nextBlur) {
            return nullptr;
        }
        tmpBlur = std::move(nextBlur);
    }

    const float sigmaScale = blurRadius * kInputScale * 0.5f;

    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);

    sk_sp<SkImageFilter> finalFilter = SkImageFilters::Blur(
        sigmaScale, sigmaScale, SkTileMode::kMirror, nullptr);

    paint.setImageFilter(finalFilter);

    sk_sp<SkSurface> surface = SkSurface::MakeRenderTarget(context, skgpu::Budgeted::kNo, scaledInfo);

    surface->getCanvas()->drawImage(tmpBlur.get(), 0, 0);

    return surface->makeImageSnapshot();
}

} // namespace skia
} // namespace renderengine
} // namespace android
