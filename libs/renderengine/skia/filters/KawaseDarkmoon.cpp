/*
 * Copyright 2026 KamiKaonashi
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define _USE_MATH_DEFINES

#include "KawaseDarkmoon.h"

#include <algorithm>
#include <cmath>

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkColorType.h>
#include <SkData.h>
#include <SkImageInfo.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRect.h>
#include <SkRuntimeEffect.h>
#include <SkSamplingOptions.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/trace.h>
#include <log/log.h>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

static constexpr float kQuarterResMinRadius = 6.0f;

const SkString kEffectSource_KawaseDarkmoon_QuarterResDownSampleBlurEffect(R"(
uniform shader child;

const float2 STEP_0 = float2( 0.25,  0.25);
const float2 STEP_1 = float2( 0.25, -0.25);
const float2 STEP_2 = float2(-0.25, -0.25);
const float2 STEP_3 = float2(-0.25,  0.25);

half4 main(float2 xy) {
    half3 c  = child.eval(xy + STEP_0).rgb;
    c += child.eval(xy + STEP_1).rgb;
    c += child.eval(xy + STEP_2).rgb;
    c += child.eval(xy + STEP_3).rgb;
    return half4(c * 0.25, 1.0);
}
)");

const SkString kEffectSource_KawaseDarkmoon_HalfResDownSampleBlurEffect(R"(
uniform shader child;

const float2 STEP_0 = float2( 0.5,  0.5);
const float2 STEP_1 = float2( 0.5, -0.5);
const float2 STEP_2 = float2(-0.5, -0.5);
const float2 STEP_3 = float2(-0.5,  0.5);

half4 main(float2 xy) {
    half3 c  = child.eval(xy + STEP_0).rgb;
    c += child.eval(xy + STEP_1).rgb;
    c += child.eval(xy + STEP_2).rgb;
    c += child.eval(xy + STEP_3).rgb;
    return half4(c * 0.25, 1.0);
}
)");

const SkString kEffectSource_KawaseDarkmoon_UpSampleBlurEffect(R"(
uniform shader child;
uniform half in_blurOffset;
uniform half in_crossFade;
uniform half in_weightedCrossFade;

half4 main(float2 xy) {
    float off = float(in_blurOffset);

    half3 c = child.eval(xy).rgb * 4.0;
    c += child.eval(xy + float2( off,  off)).rgb;
    c += child.eval(xy + float2( off, -off)).rgb;
    c += child.eval(xy + float2(-off, -off)).rgb;
    c += child.eval(xy + float2(-off,  off)).rgb;

    return half4(c * in_weightedCrossFade, in_crossFade);
}
)");

KawaseDarkmoon::KawaseDarkmoon(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager) {
    mQuarterResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseDarkmoon_QuarterResDownSampleBlurEffect];
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseDarkmoon_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseDarkmoon_UpSampleBlurEffect];

    for (int i = 0; i < kMaxSurfaces; i++) {
        mCachedSurfaceSizes[i] = SkISize::MakeEmpty();
    }
}

void KawaseDarkmoon::blurInto(const sk_sp<SkSurface>& drawSurface,
                               const sk_sp<SkImage>& readImage, const float radius,
                               const float alpha,
                               const sk_sp<SkRuntimeEffect>& blurEffect) const {
    const float scale = static_cast<float>(drawSurface->width()) / readImage->width();
    SkMatrix blurMatrix = SkMatrix::Scale(scale, scale);
    blurInto(drawSurface,
             readImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                   SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                   blurMatrix),
             radius, alpha, blurEffect);
}

void KawaseDarkmoon::blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input,
                               const float radius, const float alpha,
                               const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkPaint paint;
    if (blurEffect == mUpSampleBlurEffect) {
        if (radius == 0) {
            paint.setShader(std::move(input));
            paint.setAlphaf(alpha);
        } else {
            SkRuntimeShaderBuilder blurBuilder(blurEffect);
            blurBuilder.child("child")                  = std::move(input);
            blurBuilder.uniform("in_crossFade")         = alpha;
            blurBuilder.uniform("in_weightedCrossFade") = alpha * 0.125f;
            blurBuilder.uniform("in_blurOffset")        = radius;
            paint.setShader(blurBuilder.makeShader(nullptr));
        }
    } else {
        SkRuntimeShaderBuilder blurBuilder(blurEffect);
        blurBuilder.child("child") = std::move(input);
        paint.setShader(blurBuilder.makeShader(nullptr));
    }
    paint.setBlendMode(alpha == 1.0f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    drawSurface->getCanvas()->drawPaint(paint);
}

sk_sp<SkImage> KawaseDarkmoon::generateTemporaryImage(SkiaGpuContext* context,
                                                      const DisplaySettings& /*display*/,
                                                      const uint32_t radius,
                                                      const sk_sp<SkImage> blurInput,
                                                      const SkRect& blurRect) const {
    LOG_ALWAYS_FATAL_IF(context == nullptr, "%s: Needs GPU context", __func__);
    LOG_ALWAYS_FATAL_IF(blurInput == nullptr, "%s: Invalid input image", __func__);

    if (radius == 0) {
        return blurInput;
    }

    const float transformedRadius = std::min(radius * 0.57735f, 17.5f);
    const float filterDepth       = std::min(kMaxSurfaces - 1.0f, transformedRadius * kInputScale / 2.5f);
    const int   filterPasses      =
            std::min(kMaxSurfaces - 1, static_cast<int>(ceil(filterDepth)));

    const bool useQuarterRes = transformedRadius > kQuarterResMinRadius;

#ifndef NDEBUG
    if (blurInput->colorType() == kRGBA_F16_SkColorType ||
        blurInput->colorType() == kRGBA_F32_SkColorType) {
        ALOGW("%s: Input surface is float/HDR (%d) but blur intermediates are "
              "kRGBA_8888. Wide-gamut precision will be lost.",
              __func__, static_cast<int>(blurInput->colorType()));
    }
#endif

    SkIRect targetBlurRect;
    blurRect.roundIn(&targetBlurRect);

    if (mCachedContext != context) {
        ALOGD("%s: SkiaGpuContext changed (%p -> %p), invalidating surface cache.",
              __func__, static_cast<const void*>(mCachedContext), static_cast<const void*>(context));
        for (int i = 0; i < kMaxSurfaces; i++) {
            mCachedSurfaces[i].reset();
            mCachedSurfaceSizes[i] = SkISize::MakeEmpty();
        }
        mCachedContext = context;
    }

    auto ensureSurface = [&](float scale, int idx) -> bool {
        const int newW = std::max(1, static_cast<int>(
                static_cast<float>(targetBlurRect.width()) / scale));
        const int newH = std::max(1, static_cast<int>(
                static_cast<float>(targetBlurRect.height()) / scale));
        const SkISize sz = SkISize::Make(newW, newH);

        if (mCachedSurfaces[idx] && mCachedSurfaceSizes[idx] == sz) {
            return true;
        }

        SkImageInfo info = blurInput->imageInfo()
                                .makeColorType(kRGBA_8888_SkColorType)
                                .makeWH(newW, newH);

        mCachedSurfaces[idx] = context->createRenderTarget(info);
        if (!mCachedSurfaces[idx]) {
            ALOGE("%s: OOM - failed to allocate blur surface[%d] (%dx%d). "
                  "Returning unblurred input.",
                  __func__, idx, newW, newH);
            return false;
        }
        mCachedSurfaceSizes[idx] = sz;
        return true;
    };

    for (int i = 0; i <= filterPasses; i++) {
        const float baseScale = !useQuarterRes ? 0.5f : 1.0f;
        const float scale = static_cast<float>(1 << i) * kInverseInputScale * baseScale;
        if (!ensureSurface(scale, i)) {
            for (int j = 0; j < kMaxSurfaces; j++) {
                mCachedSurfaces[j].reset();
                mCachedSurfaceSizes[j] = SkISize::MakeEmpty();
            }
            mCachedContext = nullptr;
            return blurInput;
        }
    }

    float sumSquaredR    = 0;
    float sumSquaredStep = 0;
    for (int i = 0; i < filterPasses; i++) {
        const float alpha = std::min(1.0f, filterDepth - i);
        sumSquaredR    += powf(powf(2.0f, static_cast<float>(i - 1)) * alpha * static_cast<float>(M_SQRT2), 2.0f);
        sumSquaredStep += powf(powf(2.0f, static_cast<float>(i)) * alpha, 2.0f);
    }
    const float step = sqrtf(
            std::max(0.0f, powf(transformedRadius * kInputScale, 2.0f) - sumSquaredR) /
            (sumSquaredStep == 0.0f ? 1.0f : sumSquaredStep));

    sk_sp<SkImage> snapshots[kMaxSurfaces] = {};

    SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
    blurMatrix.postScale(kInputScale, kInputScale);
    auto sourceShader =
            blurInput->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                  SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                  blurMatrix);

    const auto& firstPassEffect = useQuarterRes ? mQuarterResDownSampleBlurEffect
                                                : mHalfResDownSampleBlurEffect;
    blurInto(mCachedSurfaces[0], std::move(sourceShader), 0, 1.0f, firstPassEffect);
    if (filterPasses > 0) {
        snapshots[0] = mCachedSurfaces[0]->makeImageSnapshot();
    }

    for (int i = 0; i < filterPasses; i++) {
        blurInto(mCachedSurfaces[i + 1], snapshots[i], 0, 1.0f,
                 mHalfResDownSampleBlurEffect);
        snapshots[i + 1] = mCachedSurfaces[i + 1]->makeImageSnapshot();
    }

    for (int i = filterPasses - 1; i >= 0; i--) {
        blurInto(mCachedSurfaces[i], snapshots[i + 1], step,
                 std::min(1.0f, filterDepth - i), mUpSampleBlurEffect);
        if (i > 0) {
            snapshots[i] = mCachedSurfaces[i]->makeImageSnapshot();
        }
    }

    return mCachedSurfaces[0]->makeImageSnapshot();
}

} // namespace skia
} // namespace renderengine
} // namespace android
