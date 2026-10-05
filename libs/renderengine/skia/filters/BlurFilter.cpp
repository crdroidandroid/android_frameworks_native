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
#include "BlurFilter.h"
#include <SkBlendMode.h>
#include <SkM44.h>
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/ThreadStateCrashLogger.h>
#include <common/trace.h>
#include <log/log.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

const SkString kEffectSource_BlurFilter_MixEffect(R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float mixFactor;

    half4 main(float2 xy) {
        return half4(mix(originalInput.eval(xy), blurredInput.eval(xy), mixFactor)).rgb1;
    }
)");

// PenguinOS Liquid Glass. A rounded blur region is drawn as a slab of glass: towards its rim the
// glass curves like a lens and bends what's behind it inwards, the colours behind it are lifted,
// and its rim catches the light, brightest along the top left.
const SkString kEffectSource_LiquidGlassEffect(R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float clarity;
    uniform float4 bounds;
    uniform float radius;
    uniform float band;
    uniform float refraction;
    uniform float rim;
    uniform float saturation;
    uniform float gain;
    uniform float lift;
    uniform float edgeClear;
    uniform float curve;

    float roundRect(float2 p, float2 c, float2 h, float r) {
        float2 q = abs(p - c) - (h - r);
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    }

    half4 main(float2 xy) {
        float2 c = (bounds.xy + bounds.zw) * 0.5;
        float2 h = (bounds.zw - bounds.xy) * 0.5;
        float r = min(radius, min(h.x, h.y));
        float d = roundRect(xy, c, h, r);
        // The outward normal straight from the distance field's own terms, rather than by four
        // more evaluations of it per pixel.
        float2 rel = xy - c;
        float2 q = abs(rel) - (h - r);
        float2 n = (q.x > 0.0 && q.y > 0.0) ? normalize(q)
                 : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
        n *= float2(rel.x < 0.0 ? -1.0 : 1.0, rel.y < 0.0 ? -1.0 : 1.0);

        // 0 across the flat middle, 1 at the very edge.
        float t = clamp(1.0 + d / band, 0.0, 1.0);
        // The rim bends what's behind it as a lens would: along a ramp, or as a dome, steepest
        // right at the edge where the glass curves away.
        float dome = 1.0 - sqrt(max(1.0 - t * t, 0.0));
        float bend = refraction * mix(t * t * t, dome, curve);
        float2 src = xy - n * bend;
        // Frosted across the middle so what sits on the glass stays legible, clearer towards the
        // rim, where the bent picture of what's behind shows through.
        float clear = clarity + (1.0 - clarity) * edgeClear * t * t;
        half3 rgb = blurredInput.eval(src).rgb;
        // Most of a slab is its flat middle, where nothing of the sharp backdrop shows.
        if (clear > 0.002) {
            rgb = mix(rgb, originalInput.eval(src).rgb, half(clear));
        }

        half l = dot(rgb, half3(0.2126, 0.7152, 0.0722));
        rgb = mix(half3(l), rgb, half(saturation));
        rgb = rgb * half(gain) + half(lift);

        float facing = dot(n, normalize(float2(-0.6, -1.0)));
        float edge = pow(t, 6.0);
        rgb += half(rim * edge * (0.55 + 0.45 * facing));
        rgb += half(rim * 0.35 * edge * max(-facing, 0.0));
        return half4(clamp(rgb, 0.0, 1.0), 1.0);
    }
)");

const SkString kEffectSource_CosGlassEffect(R"(
    uniform shader blurredInput;
    uniform half4 modes;
    uniform half4 color0;
    uniform half4 color1;
    uniform half4 color2;
    uniform half4 color3;
    uniform half4 lift;
    uniform float4 bounds;
    uniform float radius;
    uniform float edge[16];
    uniform float glow[9];

    float rbox(float2 p, float2 b, float r) {
        float2 q = abs(p) - b + r;
        return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
    }
    float getDis(float2 pos, float2 halfSize) {
        float2 rect = abs(pos - halfSize);
        return rect.x + rect.y;
    }
    float random(float2 st) {
        return fract(sin(dot(st, float2(12.9898, 78.233))) * 43758.5453);
    }
    float softLight(float s, float d) {
        return s <= 0.5 ? d - (1.0 - 2.0 * s) * d * (1.0 - d) : d + (2.0 * s - 1.0) * (sqrt(d) - d);
    }
    half3 innerGlow(float2 xy, float2 pos, float2 halfSize, float corner, float d, half3 color) {
        float gray = clamp((dot(float3(0.2126, 0.7152, 0.0722), color) - 0.6) * 1.6, 0.0, 1.0);
        float2 fade = float2(glow[0], glow[1]) * (1.0 - gray) * (1.0 - gray);
        if (fade.x < 1e-4 && fade.y < 1e-4) {
            return color;
        }
        float fadeLine = -glow[2];
        float2 halfFade = halfSize * float2(glow[5], glow[6]);
        float fadeCorner = min(min(halfFade.x, halfFade.y), max(glow[2] * 1.2, corner));
        float fadeSdf = rbox(pos - float2(glow[3], glow[4]), halfFade, fadeCorner);
        float alpha = smoothstep(fadeLine, 0.0, fadeSdf) * smoothstep(fadeLine, 0.0, d);
        if (glow[8] > 1e-4) {
            alpha = pow(alpha, glow[7]) * glow[8] + (1.0 - glow[8]) * alpha;
        }
        alpha = alpha * (fade.x - fade.y) + fade.y;
        float n = clamp((random(xy * 0.013) - 0.5) * 0.008 + 0.5, 0.0, 1.0);
        alpha = clamp(softLight(n, alpha), 0.0, 1.0);
        return saturate(mix(color, color + 1.0, alpha));
    }
    half3 rimLight(float2 pos, float2 halfSize, float d, half3 color, half3 raw) {
        float2 lineNear = float2(edge[3], edge[4]);
        float2 lineFar = float2(edge[5], edge[6]);
        if (d < -max(lineNear.y, lineFar.y)) {
            return color;
        }
        float distanceAll = (halfSize.x + halfSize.y) * 2.0;
        float ratio = mod(edge[13] + halfSize.x / distanceAll * 0.5 + 0.5, 1.0);
        float2 dirAbs = abs(normalize(pos));
        float2 posN = min(float2(dirAbs.xy * halfSize.yx / max(float2(0.001), dirAbs.yx)), halfSize)
                * sign(pos);
        float flag = mix(-1.0, 1.0, step(ratio, 0.5)) * dot(float2(-halfSize.y, halfSize.x), posN);
        float disPoint = distanceAll * min(ratio, 1.0 - ratio) * 2.0;
        float disPos = getDis(posN, halfSize);
        float near = mix(disPoint + disPos, abs(disPoint - disPos), step(0.0, flag));
        near = min(2.0 * distanceAll - near, near);
        float nearAlpha = edge[7] * smoothstep(edge[10] * distanceAll, edge[9] * distanceAll, near);
        float farAlpha =
                edge[8] * smoothstep(edge[12] * distanceAll, edge[11] * distanceAll, distanceAll - near);
        float nearSdf = smoothstep(-lineNear.y, -lineNear.x, d);
        float farSdf = smoothstep(-lineFar.y, -lineFar.x, d);
        if (edge[15] > 1e-4) {
            nearSdf = pow(nearSdf, edge[14]) * edge[15] + (1.0 - edge[15]) * nearSdf;
            farSdf = pow(farSdf, edge[14]) * edge[15] + (1.0 - edge[15]) * farSdf;
        }
        float lineAlpha = clamp(nearAlpha * nearSdf + farAlpha * farSdf, 0.0, 1.0);
        float gray = clamp(dot(float3(0.2126, 0.7152, 0.0722), raw) * 1.2, 0.0, 1.0);
        float improve = (1.0 - gray) * (1.0 - gray);
        improve = improve * improve * 0.1;
        half3 lit = saturate(raw + improve);
        half3 lineColor = half3(edge[0], edge[1], edge[2]);
        lit = mix(2.0 * lit * lineColor, 1.0 - 2.0 * (1.0 - lit) * (1.0 - lineColor),
                  step(lineColor, half3(0.5)));
        return mix(color, saturate(lit), lineAlpha);
    }

    half overlay(half s, half d) {
        return (d < 0.5) ? 2.0 * s * d : 1.0 - 2.0 * (1.0 - s) * (1.0 - d);
    }
    half3 overlay(half3 s, half3 d) {
        return half3(overlay(s.x, d.x), overlay(s.y, d.y), overlay(s.z, d.z));
    }
    half colorDodge(half s, half d) { return d / (1.0 - s); }
    half3 colorDodge(half3 s, half3 d) {
        return half3(colorDodge(s.x, d.x), colorDodge(s.y, d.y), colorDodge(s.z, d.z));
    }
    half3 luminosity(half3 s, half3 d) {
        half dLum = dot(d, half3(0.3, 0.59, 0.11));
        half sLum = dot(s, half3(0.3, 0.59, 0.11));
        half3 c = d + (sLum - dLum);
        half minC = min(min(c.x, c.y), c.z);
        half maxC = max(max(c.x, c.y), c.z);
        if (minC < 0.0) {
            return sLum + ((c - sLum) * sLum) / (sLum - minC);
        } else if (maxC > 1.0) {
            return sLum + ((c - sLum) * (1.0 - sLum)) / (maxC - sLum);
        }
        return c;
    }
    half3 layer(half3 col, half mode, half4 c) {
        if (mode == 1) {
            col = mix(col, c.rgb, c.a);
        } else if (mode == 2) {
            col = mix(col, overlay(c.rgb, col), c.a);
        } else if (mode == 3) {
            col = mix(col, colorDodge(c.rgb, col), c.a);
        } else if (mode == 4) {
            col = mix(col, luminosity(c.rgb, col), c.a);
            col = mix(col, overlay(c.rgb, col), c.a);
        } else if (mode == 5) {
            col = mix(col, luminosity(c.rgb, col), c.a);
            col = mix(col, colorDodge(c.rgb, col), c.a);
        } else if (mode == 6) {
            // Neutral glass retains dark/saturated content instead of washing it with grey.
            half luminance = dot(col, half3(0.2126, 0.7152, 0.0722));
            col *= mix(half3(1), c.rgb, c.a * smoothstep(0.2, 1.0, luminance));
        }
        return col;
    }
    half4 main(float2 xy) {
        half4 col = blurredInput.eval(xy);
        half3 rgb = layer(col.rgb, modes.x, color0);
        rgb = layer(rgb, modes.y, color1);
        rgb = layer(rgb, modes.z, color2);
        rgb = layer(rgb, modes.w, color3);
        rgb = rgb * lift.x + lift.y;
        half luma = dot(rgb, half3(0.2126, 0.7152, 0.0722));
        rgb = saturate(mix(half3(luma), rgb, lift.z));
        if (edge[7] + edge[8] > 0.0 || glow[0] > 0.0) {
            float2 halfSize = (bounds.zw - bounds.xy) * 0.5;
            float2 pos = xy - (bounds.xy + halfSize);
            float corner = min(radius, min(halfSize.x, halfSize.y));
            float d = rbox(pos, halfSize, corner);
            rgb = innerGlow(xy, pos, halfSize, corner, d, rgb);
            rgb = rimLight(pos, halfSize, d, rgb, col.rgb);
        }
        return half4(rgb, 1.0);
    }
)");

static constexpr const char* kLiquidGlassProperty = "persist.sys.evolution.liquid_glass";
// The names of the screens running a DeX desktop, separated by '|', which have Liquid Glass even
// with the system switch off.
static constexpr const char* kDexGlassProperty = "sys.evolution.liquid_glass.dex";
static constexpr const char* kLiquidGlassStyleProperty = "persist.sys.evolution.liquid_glass_style";

// A system property's value, re-read only when it changes.
class CachedProperty {
public:
    explicit CachedProperty(const char* name) : mName(name) {}

    const std::string& get() {
        if (mInfo == nullptr) {
            const uint32_t area = __system_property_area_serial();
            if (area == mAreaSerial) return mValue;
            mAreaSerial = area;
            mInfo = __system_property_find(mName);
            if (mInfo == nullptr) return mValue;
        }
        const uint32_t serial = __system_property_serial(mInfo);
        if (serial != mSerial) {
            mSerial = serial;
            __system_property_read_callback(
                    mInfo,
                    [](void* cookie, const char*, const char* value, uint32_t) {
                        *static_cast<std::string*>(cookie) = value;
                    },
                    &mValue);
        }
        return mValue;
    }

private:
    const char* mName;
    const prop_info* mInfo = nullptr;
    uint32_t mAreaSerial = UINT32_MAX;
    uint32_t mSerial = UINT32_MAX;
    std::string mValue;
};

static bool cosGlassStyle() {
    static CachedProperty style(kLiquidGlassStyleProperty);
    return style.get() == "cos";
}

// Whether Penguin Laboratory's Liquid Glass is on, or the display is one of DeX's desktops, where
// it always is. namePlusId is "<display name> (<id>)", after "ScreenCapture, " for screenshots.
static bool liquidGlassEnabledFor(const std::string& namePlusId) {
    static CachedProperty global(kLiquidGlassProperty);
    static CachedProperty dex(kDexGlassProperty);
    const std::string& on = global.get();
    if (on == "1" || on == "true") return true;
    const std::string& names = dex.get();
    size_t start = 0;
    while (start < names.size()) {
        size_t end = names.find('|', start);
        if (end == std::string::npos) end = names.size();
        if (end > start) {
            // Compositing names the display alone; screenshots put "ScreenCapture, " first.
            const std::string name = names.substr(start, end - start) + " (";
            if (namePlusId.compare(0, name.size(), name) == 0 ||
                namePlusId.find(", " + name) != std::string::npos) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

// A Liquid Glass value overridden live from debug.evolution.lg.sf.<name>, for matching the material by eye.
static float tune(const char* name, float fallback) {
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<CachedProperty>> props;
    std::lock_guard<std::mutex> guard(lock);
    auto& prop = props[name];
    if (prop == nullptr) {
        prop = std::make_unique<CachedProperty>(
                strdup((std::string("debug.evolution.lg.sf.") + name).c_str()));
    }
    const std::string& value = prop->get();
    if (value.empty()) return fallback;
    char* end = nullptr;
    const float parsed = strtof(value.c_str(), &end);
    return end != value.c_str() ? parsed : fallback;
}

void BlurFilter::setDisplay(const std::string& namePlusId) {
    mLiquidGlass = mLiquidGlassEffect != nullptr && liquidGlassEnabledFor(namePlusId);
}

struct CosGlassLayers {
    SkV4 modes;
    SkV4 colors[4];
};

static SkV4 argb(uint32_t c) {
    return SkV4{((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f,
                (c >> 24) / 255.0f};
}

static const CosGlassLayers& cosGlassLayers(int32_t material) {
    static const CosGlassLayers inactive{SkV4{5, 2, 5, 3},
                                         {argb(0xA6000000), argb(0x33999999), argb(0x40404040),
                                          argb(0x667B7B7B)}};
    static const CosGlassLayers active{SkV4{5, 2, 5, 3},
                                       {argb(0xA6000000), argb(0x33999999), argb(0x7DE6E6E6),
                                        argb(0x40CCCCCC)}};
    static const CosGlassLayers lightInactive{SkV4{5, 2, 5, 3},
                                              {argb(0x92000000), argb(0x66999999),
                                               argb(0x19404040), argb(0x4D737373)}};
    static const CosGlassLayers lightActive{SkV4{5, 2, 5, 3},
                                            {argb(0x92000000), argb(0x66999999), argb(0x73E6E6E6),
                                             argb(0x40CCCCCC)}};
    static const CosGlassLayers panel{SkV4{5, 2, 0, 0},
                                      {argb(0xA6000000), argb(0x33999999), argb(0),
                                       argb(0)}};
    static const CosGlassLayers lightPanel{SkV4{5, 2, 0, 0},
                                           {argb(0x92000000), argb(0x66999999), argb(0),
                                            argb(0)}};
    static const CosGlassLayers card{SkV4{5, 3, 0, 0},
                                     {argb(0x59404040), argb(0x66737373), argb(0), argb(0)}};
    static const CosGlassLayers lightCard{SkV4{5, 3, 0, 0},
                                          {argb(0x19404040), argb(0x4D737373), argb(0),
                                           argb(0)}};
    static const CosGlassLayers headsUp{SkV4{5, 3, 0, 0},
                                        {argb(0x8C595959), argb(0xFF333333), argb(0), argb(0)}};
    static const CosGlassLayers lightHeadsUp{SkV4{5, 3, 0, 0},
                                             {argb(0x80BFBFBF), argb(0xCC333333), argb(0),
                                              argb(0)}};
    // Explicit opt-in dock material. Keep the regular COS materials and shade unchanged.
    static const CosGlassLayers dock{SkV4{6, 0, 0, 0},
                                     {argb(0xFFD4D4D4), argb(0), argb(0), argb(0)}};
    switch (material) {
        case 11:
            return dock;
        case 2:
            return active;
        case 7:
            return card;
        case 8:
            return lightCard;
        case 9:
            return headsUp;
        case 10:
            return lightHeadsUp;
        case 5:
            return panel;
        case 6:
            return lightPanel;
        case 3:
            return lightInactive;
        case 4:
            return lightActive;
        default:
            return inactive;
    }
}

struct CosRimTemplate {
    float nearWidth;
    float farWidth;
    float alphaNear;
    float alphaFar;
    float fade[4];
    float glowAlpha;
    float glowOffset;
};

static const CosRimTemplate& cosRimTemplate(int32_t material) {
    static const CosRimTemplate inactiveDark{7, 4, 0.35f, 0.4f, {0.25f, 0.6f, 0.15f, 0.5f}, 0.07f,
                                             25};
    static const CosRimTemplate inactiveLight{7, 4, 0.25f, 0.6f, {0.25f, 0.6f, 0.15f, 0.5f},
                                              0.15f, 36};
    static const CosRimTemplate active{8, 2, 0.5f, 0.5f, {0.3f, 0.6f, 0.3f, 0.6f}, 0.15f, 36};
    static const CosRimTemplate headsUp{7, 4, 0.4f, 0.4f, {0.2f, 0.5f, 0.2f, 0.5f}, 0.07f, 25};
    static const CosRimTemplate lightHeadsUp{7, 4, 0.8f, 0.8f, {0.2f, 0.5f, 0.2f, 0.5f}, 0.15f,
                                             36};
    switch (material) {
        case 2:
        case 4:
            return active;
        case 9:
            return headsUp;
        case 10:
            return lightHeadsUp;
        case 3:
        case 8:
            return inactiveLight;
        default:
            return inactiveDark;
    }
}

static float cosRimRatio(float w, float h, float radius) {
    if (w <= 0 || h <= 0) return 0.5f;
    const float shortSide = std::min(w, h);
    const float corner = std::min(radius, shortSide / 2);
    const float side = (w * h - shortSide * shortSide) / (2 * shortSide);
    float pos = w >= h ? side + h / 2 : h / 2 - side;
    if (!(w == h && radius >= h / 2)) pos -= corner / 2;
    return pos * 0.5f / (w + h) + 0.5f;
}

static SkMatrix getShaderTransform(const SkCanvas* canvas, const SkRect& blurRect,
                                   const float scale, const float zoomScale) {
    // 1. Apply the blur shader matrix, which scales up the blurred surface to its real size
    auto matrix = SkMatrix::Scale(scale, scale);
    // 2. Since the blurred surface has the size of the layer, we align it with the
    // top left corner of the layer position.
    matrix.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop));
    // 3. Apply the "zoom" effect as an extra scale + translate around the center of the blur.
    if (zoomScale != 1.0f) {
        matrix.postScale(zoomScale, zoomScale);
        matrix.postTranslate(
                blurRect.width() * (1 - zoomScale) / 2.0f,
                blurRect.height() * (1 - zoomScale) / 2.0f);
    }
    // 4. Finally, apply the inverse canvas matrix. The snapshot made in the BlurFilter is in the
    // original surface orientation. The inverse matrix has to be applied to align the blur
    // surface with the current orientation/position of the canvas.
    SkMatrix drawInverse;
    if (canvas != nullptr && canvas->getTotalMatrix().invert(&drawInverse)) {
        matrix.postConcat(drawInverse);
    }
    return matrix;
}

BlurFilter::BlurFilter(RuntimeEffectManager& effectManager, const float maxCrossFadeRadius)
      : mMaxCrossFadeRadius(maxCrossFadeRadius),
        mMixEffect(effectManager.mKnownEffects[kBlurFilter_MixEffect]),
        mLiquidGlassEffect(effectManager.mKnownEffects[kLiquidGlassEffect]),
        mCosGlassEffect(effectManager.mKnownEffects[kCosGlassEffect]) {}

float BlurFilter::getMaxCrossFadeRadius() const {
    return mMaxCrossFadeRadius;
}

void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                const uint32_t blurRadius, const float zoomScale,
                                const float blurAlpha,
                                const SkRect& blurRect, sk_sp<SkImage> blurredImage,
                                sk_sp<SkImage> input, int32_t material) {
    SFTRACE_CALL();

    SkPaint paint;
    paint.setAlphaf(blurAlpha);

    auto blurMatrix = getShaderTransform(canvas, blurRect, kInverseInputScale, zoomScale);

    SkSamplingOptions linearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    const auto blurShader = blurredImage->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                                     linearSampling, &blurMatrix);

    if (material == 0 && mLiquidGlass && mCosGlassEffect != nullptr && cosGlassStyle()) {
        material = effectRegion.isRect() ? 5 : 1;
    }

    if (material > 0 && mCosGlassEffect != nullptr) {
        SkRuntimeShaderBuilder glass(mCosGlassEffect);
        glass.child("blurredInput") = blurShader;
        const CosGlassLayers& layers = cosGlassLayers(material);
        glass.uniform("modes") = layers.modes;
        glass.uniform("color0") = layers.colors[0];
        glass.uniform("color1") = layers.colors[1];
        glass.uniform("color2") = layers.colors[2];
        glass.uniform("color3") = layers.colors[3];
        glass.uniform("lift") = material == 11 ? SkV4{1.0f, 0.0f, 1.0f, 0.0f}
                : SkV4{tune("cos_gain", 1.0f), tune("cos_add", 0.0f),
                       tune("cos_sat", 1.0f), 0.0f};
        const SkRect& rect = effectRegion.rect();
        const float corner = effectRegion.radii(SkRRect::kUpperLeft_Corner).fX;
        float edge[16] = {};
        float glow[9] = {};
        if ((material >= 1 && material <= 4) || (material >= 7 && material <= 10)) {
            const CosRimTemplate& rim = cosRimTemplate(material);
            const float px = tune("cos_rim_px", 0.875f);
            const float strength = tune("cos_rim", 1.0f);
            const float edgeValues[16] = {1, 1, 1,
                                          rim.nearWidth * px, rim.nearWidth * px * 2,
                                          rim.farWidth * px, rim.farWidth * px * 2,
                                          rim.alphaNear * strength, rim.alphaFar * strength,
                                          rim.fade[0], rim.fade[1], rim.fade[2], rim.fade[3],
                                          cosRimRatio(rect.width(), rect.height(), corner), 10,
                                          0.5f};
            std::copy(std::begin(edgeValues), std::end(edgeValues), edge);
            const float glowValues[9] = {rim.glowAlpha * tune("cos_glow", 1.0f), 0, 40 * px, 0,
                                         rim.glowOffset * px, 1.3f, 1.3f, 20, 0.5f};
            std::copy(std::begin(glowValues), std::end(glowValues), glow);
        }
        glass.uniform("bounds") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
        glass.uniform("radius") = corner;
        glass.uniform("edge").set(edge, 16);
        glass.uniform("glow").set(glow, 9);
        paint.setShader(glass.makeShader());
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
        return;
    }

    // Liquid Glass: rounded regions become glass slabs; whole-window blurs, such as the shade's,
    // keep the colours behind them vivid instead of greying them out, as iOS's materials do.
    if (mLiquidGlass && material >= 0 && (!effectRegion.isRect() || blurRadius >= mMaxCrossFadeRadius)) {
        const bool slab = !effectRegion.isRect();
        const SkRect& rect = effectRegion.rect();
        const float radius = slab ? effectRegion.radii(SkRRect::kUpperLeft_Corner).fX : 0.0f;
        const float shortSide = std::min(rect.width(), rect.height());
        // The curved rim is as deep as the corner, but never past the middle of the glass.
        const float band =
                slab ? std::clamp(radius * tune("band", 0.6f), 6.0f, shortSide * 0.5f) : 1.0f;
        SkRuntimeShaderBuilder glass(mLiquidGlassEffect);
        glass.child("blurredInput") = blurShader;
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }
        glass.child("originalInput") =
                input != nullptr
                        ? input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                            linearSampling, inputMatrix)
                        : blurShader;
        glass.uniform("clarity") = tune("clarity", 0.0f);
        glass.uniform("bounds") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
        glass.uniform("radius") = radius;
        glass.uniform("band") = band;
        glass.uniform("refraction") = slab ? band * tune("refraction", 0.5f) : 0.0f;
        glass.uniform("rim") = slab ? tune("rim", 0.0f) : 0.0f;
        // A whole window's backdrop, such as the shade's, gets iOS's vivid blur.
        glass.uniform("saturation") = slab ? tune("slab_sat", 1.1f) : tune("window_sat", 0.5f);
        glass.uniform("gain") = slab ? tune("slab_gain", 0.9f) : tune("window_gain", 1.04f);
        glass.uniform("lift") = slab ? tune("slab_lift", 0.0f) : tune("window_lift", 0.02f);
        glass.uniform("edgeClear") = tune("edge_clear", 0.08f);
        glass.uniform("curve") = tune("curve", 1.0f);
        paint.setShader(glass.makeShader());
        if (slab) {
            paint.setAntiAlias(true);
            canvas->drawRRect(effectRegion, paint);
        } else {
            if (blurAlpha == 1.0f) {
                paint.setBlendMode(SkBlendMode::kSrc);
            }
            canvas->drawRect(rect, paint);
        }
        return;
    }

    if (blurRadius < mMaxCrossFadeRadius) {
        LOG_THREAD_STATE_AND_CRASH_IF(!input);

        // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
        // case you might expect the matrix to simply be the canvas matrix.
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }
        if (zoomScale != 1.0f) {
            inputMatrix.preTranslate(
                    blurRect.width() * (1 - zoomScale) / 2.0f,
                    blurRect.height() * (1 - zoomScale) / 2.0f);
            inputMatrix.preScale(zoomScale, zoomScale);
        }

        SkRuntimeShaderBuilder blurBuilder(mMixEffect);
        blurBuilder.child("blurredInput") = blurShader;
        blurBuilder.child("originalInput") =
                input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linearSampling,
                                  inputMatrix);

        blurBuilder.uniform("mixFactor") = blurRadius / mMaxCrossFadeRadius;

        paint.setShader(blurBuilder.makeShader());
    } else {
        paint.setShader(blurShader);
    }

    if (effectRegion.isRect()) {
        // Penguin's plain window blurs (material -1, such as home's while an app closes) can come
        // out partly transparent at the screen edges; copying them with kSrc punched those edges
        // through to black, so blend them over what is already drawn instead.
        if (blurAlpha == 1.0f && material >= 0) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        canvas->drawRect(effectRegion.rect(), paint);
    } else {
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
