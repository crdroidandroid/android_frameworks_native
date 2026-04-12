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

#pragma once

#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkSurface.h>
#include "BlurFilter.h"
#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

/**
 * KawaseDarkmoon - Highly optimized dual-filter blur
 */
class KawaseDarkmoon : public BlurFilter {
public:
    explicit KawaseDarkmoon(RuntimeEffectManager& effectManager);
    virtual ~KawaseDarkmoon() {}

    sk_sp<SkImage> generateTemporaryImage(SkiaGpuContext* context,
                                          const DisplaySettings& display,
                                          const uint32_t radius,
                                          const sk_sp<SkImage> blurInput,
                                          const SkRect& blurRect) const override;

private:
    static constexpr int kMaxSurfaces = 3;

    sk_sp<SkRuntimeEffect> mQuarterResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mHalfResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mUpSampleBlurEffect;

    mutable SkiaGpuContext* mCachedContext                  = nullptr;
    mutable sk_sp<SkSurface> mCachedSurfaces[kMaxSurfaces]  = {};
    mutable SkISize mCachedSurfaceSizes[kMaxSurfaces]        = {};

    void blurInto(const sk_sp<SkSurface>& drawSurface,
                  const sk_sp<SkImage>&   readImage,
                  const float radius, const float alpha,
                  const sk_sp<SkRuntimeEffect>& blurEffect) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface,
                  sk_sp<SkShader>         input,
                  const float radius, const float alpha,
                  const sk_sp<SkRuntimeEffect>& blurEffect) const;
};

} // namespace skia
} // namespace renderengine
} // namespace android
