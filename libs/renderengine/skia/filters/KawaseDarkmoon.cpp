/*
 * Copyright 2026 KamiKaonashi
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

#include "KawaseDarkmoon.h"

#include <algorithm>
#include <cmath>

#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkColorSpace.h>
#include <SkColorType.h>
#include <SkImageInfo.h>
#include <SkPaint.h>
#include <SkRect.h>
#include <SkRuntimeEffect.h>
#include <SkSamplingOptions.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/ThreadStateCrashLogger.h>
#include <common/trace.h>
#include <log/log.h>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

namespace {

constexpr float kBlurSigmaScale = 0.57735f;

constexpr float kDiagonalKernelCompensation = 0.93541435f;

constexpr float kClampInset = 0.5001f;

const SkSamplingOptions kLinearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);

int levelDimension(int fullResDimension, int level) {
    const float scale = KawaseDarkmoon::kInverseInputScale * static_cast<float>(1 << level);
    return std::max(1, static_cast<int>(static_cast<float>(fullResDimension) / scale));
}

SkISize levelSize(SkISize fullRes, int level) {
    return SkISize::Make(levelDimension(fullRes.width(), level),
                         levelDimension(fullRes.height(), level));
}

SkRect insetForClamp(const SkRect& rect) {
    SkRect inset = rect.makeInset(kClampInset, kClampInset);
    if (inset.width() < 0) {
        inset.fLeft = inset.fRight = rect.centerX();
    }
    if (inset.height() < 0) {
        inset.fTop = inset.fBottom = rect.centerY();
    }
    return inset;
}

bool isOversized(const sk_sp<SkSurface>& surface, SkISize content) {
    return surface->width() > content.width() || surface->height() > content.height();
}

bool isCompatible(const SkImageInfo& a, const SkImageInfo& b) {
    return a.colorType() == b.colorType() && a.alphaType() == b.alphaType() &&
            SkColorSpace::Equals(a.colorSpace(), b.colorSpace());
}

}

const SkString kEffectSource_KawaseDarkmoon_QuarterResDownSampleBlurEffect(R"(
    uniform shader child;

    const float2 STEP_0 = float2( 0.25,  0.25);
    const float2 STEP_1 = float2( 0.25, -0.25);
    const float2 STEP_2 = float2(-0.25, -0.25);
    const float2 STEP_3 = float2(-0.25,  0.25);

    half4 main(float2 xy) {
        half3 c = child.eval(xy + STEP_0).rgb;
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
        half3 c = child.eval(xy + STEP_0).rgb;
        c += child.eval(xy + STEP_1).rgb;
        c += child.eval(xy + STEP_2).rgb;
        c += child.eval(xy + STEP_3).rgb;

        return half4(c * 0.25, 1.0);
    }
)");

const SkString kEffectSource_KawaseDarkmoon_UpSampleBlurEffect(R"(
    uniform shader child;
    uniform float in_blurOffset;
    uniform float in_crossFade;
    uniform float in_weightedCrossFade;

    half4 main(float2 xy) {
        half3 c = child.eval(xy).rgb * 4.0;
        c += child.eval(xy + float2( in_blurOffset,  in_blurOffset)).rgb;
        c += child.eval(xy + float2( in_blurOffset, -in_blurOffset)).rgb;
        c += child.eval(xy + float2(-in_blurOffset, -in_blurOffset)).rgb;
        c += child.eval(xy + float2(-in_blurOffset,  in_blurOffset)).rgb;

        return half4(c * in_weightedCrossFade, in_crossFade);
    }
)");

KawaseDarkmoon::KawaseDarkmoon(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager) {
    mQuarterResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseDarkmoon_QuarterResDownSampleBlurEffect];
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseDarkmoon_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect = effectManager.mKnownEffects[kKawaseDarkmoon_UpSampleBlurEffect];
}

bool KawaseDarkmoon::ensureSurface(SurfaceCache& cache, int index, SkiaGpuContext* context,
                                   const SkImageInfo& info, SkISize required,
                                   SkISize preferred) const {
    sk_sp<SkSurface>& surface = cache.surfaces[index];
    if (surface) {
        const SkImageInfo& current = surface->imageInfo();
        const bool compatible = isCompatible(current, info);
        if (compatible && current.width() >= required.width() &&
            current.height() >= required.height()) {
            return true;
        }
        if (compatible) {
            preferred.fWidth = std::max(preferred.width(), current.width());
            preferred.fHeight = std::max(preferred.height(), current.height());
        }
        surface.reset();
    }

    surface = context->createRenderTarget(info.makeWH(preferred.width(), preferred.height()));
    if (!surface) {
        context->purgeUnlockedScratchResources();
        surface = context->createRenderTarget(info.makeWH(preferred.width(), preferred.height()));
        if (!surface && preferred != required) {
            surface = context->createRenderTarget(info.makeWH(required.width(), required.height()));
        }
    }
    return surface != nullptr;
}

void KawaseDarkmoon::blurInto(const sk_sp<SkSurface>& drawSurface, SkISize drawContent,
                              sk_sp<SkShader> input, const SkRect* inputClampRect,
                              const SkMatrix& inputToDest, float radius, float alpha,
                              const sk_sp<SkRuntimeEffect>& blurEffect) const {
    if (inputClampRect) {
        input = SkShaders::CoordClamp(std::move(input), insetForClamp(*inputClampRect));
    }
    input = input->makeWithLocalMatrix(inputToDest);

    SkPaint paint;
    if (blurEffect == mUpSampleBlurEffect && radius == 0) {
        paint.setShader(std::move(input));
        paint.setAlphaf(alpha);
    } else {
        SkRuntimeShaderBuilder blurBuilder(blurEffect);
        blurBuilder.child("child") = std::move(input);
        if (blurEffect == mUpSampleBlurEffect) {
            blurBuilder.uniform("in_blurOffset") = radius;
            blurBuilder.uniform("in_crossFade") = alpha;
            blurBuilder.uniform("in_weightedCrossFade") = alpha * 0.125f;
        }
        paint.setShader(blurBuilder.makeShader(nullptr));
    }
    paint.setBlendMode(alpha == 1.0f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);

    SkCanvas* canvas = drawSurface->getCanvas();
    SkAutoCanvasRestore autoRestore(canvas, true);
    if (isOversized(drawSurface, drawContent)) {
        canvas->clipRect(SkRect::MakeWH(drawContent.width() + 1, drawContent.height() + 1));
    }
    canvas->drawPaint(paint);
}

sk_sp<SkImage> KawaseDarkmoon::generateTemporaryImage(SkiaGpuContext* context,
                                                      const DisplaySettings& display,
                                                      const uint32_t blurRadius,
                                                      const sk_sp<SkImage> input,
                                                      const SkRect& blurRect) const {
    SFTRACE_CALL();
    LOG_ALWAYS_FATAL_IF(context == nullptr, "%s: Needs GPU context", __func__);
    LOG_ALWAYS_FATAL_IF(input == nullptr, "%s: Invalid input image", __func__);

    if (blurRadius == 0) {
        return input;
    }

    const float radius = blurRadius * kBlurSigmaScale;

    const float filterDepth = std::min(kMaxSurfaces - 1.0f, radius * kInputScale / 2.5f);
    const int filterPasses = std::min(kMaxSurfaces - 1, static_cast<int>(std::ceil(filterDepth)));

    SkIRect targetBlurRect;
    blurRect.roundIn(&targetBlurRect);
    const SkISize fullRes = targetBlurRect.size();

    const bool isProtected = context->supportsProtectedContent();
    SurfaceCache transientCache;
    SurfaceCache& cache = isProtected ? transientCache : mUnprotectedCache;
    if (cache.context != context) {
        cache.reset();
        cache.context = context;
    }

    const SkISize displaySize = SkISize::Make(display.physicalDisplay.width(),
                                              display.physicalDisplay.height());
    const bool hintFromDisplay = !isProtected && !displaySize.isEmpty();

    const SkImageInfo baseInfo = input->imageInfo();

    SkISize contentSizes[kMaxSurfaces];
    for (int i = 0; i <= filterPasses; i++) {
        contentSizes[i] = levelSize(fullRes, i);
        SkISize preferred = contentSizes[i];
        if (hintFromDisplay) {
            const SkISize displayLevel = levelSize(displaySize, i);
            preferred.fWidth = std::max(preferred.width(), displayLevel.width());
            preferred.fHeight = std::max(preferred.height(), displayLevel.height());
        }
        const bool allocated =
                ensureSurface(cache, i, context, baseInfo, contentSizes[i], preferred);
        LOG_THREAD_STATE_AND_CRASH_IF(!allocated,
                                      "%s: Failed to create surface %d (%dx%d) for blurring!",
                                      __func__, i, contentSizes[i].width(),
                                      contentSizes[i].height());
    }
    const sk_sp<SkSurface>* surfaces = cache.surfaces;

    float sumSquaredR = 0;
    float sumSquaredStep = 0;
    for (int i = 0; i < filterPasses; i++) {
        const float alpha = std::min(1.0f, filterDepth - i);
        sumSquaredR += powf(powf(2.0f, i - 1) * alpha * static_cast<float>(M_SQRT2), 2.0f);
        sumSquaredStep += powf(powf(2.0f, i) * alpha, 2.0f);
    }
    const float step = kDiagonalKernelCompensation *
            sqrtf(std::max(0.0f, powf(radius * kInputScale, 2.0f) - sumSquaredR) /
                  (sumSquaredStep == 0 ? 1.0f : sumSquaredStep));

    // First pass: 4x down-sample from the input.
    {
        // Skia's local matrix maps shader space -> destination, hence translate-then-scale.
        SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        blurMatrix.postScale(kInputScale, kInputScale);
        const SkRect inputClamp = SkRect::Make(targetBlurRect);
        const bool clampInput = isOversized(surfaces[0], contentSizes[0]);
        blurInto(surfaces[0], contentSizes[0],
                 input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, kLinearSampling),
                 clampInput ? &inputClamp : nullptr, blurMatrix,
                 0, // unused: the down-sample effect hardcodes its offsets
                 1.0f, mQuarterResDownSampleBlurEffect);
    }

    // Remaining 2x down-samples.
    for (int i = 0; i < filterPasses; i++) {
        const SkISize src = contentSizes[i];
        const SkISize dst = contentSizes[i + 1];
        const SkMatrix toDest = SkMatrix::Scale(static_cast<float>(dst.width()) / src.width(),
                                                static_cast<float>(dst.height()) / src.height());
        const SkRect srcClamp = SkRect::Make(src);
        const bool clampSrc = isOversized(surfaces[i], src);
        blurInto(surfaces[i + 1], dst,
                 surfaces[i]->makeTemporaryImage()->makeShader(SkTileMode::kClamp,
                                                               SkTileMode::kClamp,
                                                               kLinearSampling),
                 clampSrc ? &srcClamp : nullptr, toDest,
                 0, // unused: the down-sample effect hardcodes its offsets
                 1.0f, mHalfResDownSampleBlurEffect);
    }

    // Blur + up-sample back to quarter resolution.
    for (int i = filterPasses - 1; i >= 0; i--) {
        const SkISize src = contentSizes[i + 1];
        const SkISize dst = contentSizes[i];
        const SkMatrix toDest = SkMatrix::Scale(static_cast<float>(dst.width()) / src.width(),
                                                static_cast<float>(dst.height()) / src.height());
        const SkRect srcClamp = SkRect::Make(src);
        const bool clampSrc = isOversized(surfaces[i + 1], src);
        blurInto(surfaces[i], dst,
                 surfaces[i + 1]->makeTemporaryImage()->makeShader(SkTileMode::kClamp,
                                                                   SkTileMode::kClamp,
                                                                   kLinearSampling),
                 clampSrc ? &srcClamp : nullptr, toDest, step, std::min(1.0f, filterDepth - i),
                 mUpSampleBlurEffect);
    }

    // The result is at kInputScale relative to blurRect and anchored at the surface's origin,
    // which is what BlurFilter::drawBlurRegion() expects (it maps with a fixed inverse scale).
    return surfaces[0]->makeTemporaryImage();
}

} // namespace skia
} // namespace renderengine
} // namespace android
