#pragma once
// Oscillator warp: how an oscillator reads its frame within one cycle. The phase p (0..1) becomes
// a warped read position q (0..1) before the table lookup, so every table, classic shape and
// imported WAV can be bent, squeezed or synced, and the amount can be modulated.
//
//   Sync      hard sync: the frame runs 1 + 15 x amount times per cycle, cut off at the cycle's end
//   Bend+     the frame's start and end squeezed toward the cycle's edges (its middle stretched)
//   Bend-     the other way: start and end stretched, the middle squeezed
//   PWM       the frame squeezed into the first part of the cycle (down to 5%), the rest holds
//   Asym+     the frame's first half played faster, the second slower (up to 10 : 1)
//   Asym-     the first half slower, the second faster
//   Mirror    a blend toward the frame played forward then backward in one cycle
//   Quantize  the read position in steps (256 down to 4 per cycle): stepped, lo-fi
//
// Band-limiting: a warp reads parts of the frame faster than the oscillator's pitch (Sync 16x,
// PWM 20x at full amount). factor is that speed-up at its steepest; renderOsc picks the mip level
// for pitch x factor, so the fast parts don't alias (Sync's cut and Quantize's steps still do,
// as they do on any synth: that is their sound).
//
// Real-time: makeWarp is a handful of operations per oscillator and chunk; warp4 a few vector
// operations per four samples, no divisions except Bend's one reciprocal estimate.
#include "simd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pf {

enum WarpMode : int { WP_OFF, WP_SYNC, WP_BEND_PLUS, WP_BEND_MINUS, WP_PWM, WP_ASYM_PLUS, WP_ASYM_MINUS, WP_MIRROR,
                      WP_QUANTIZE, WP_COUNT };

struct Warp {
    int   mode = WP_OFF;
    float amt = 0.0f;       // 0..1
    float r = 1.0f;         // Sync: frame repeats per cycle; Quantize: steps per cycle
    float invR = 1.0f;      // Quantize: 1 / steps
    float c = 1.0f;         // Bend: the curve (c < 1: Bend+, c > 1: Bend-)
    float m = 0.5f;         // Asym: where the frame's middle lands
    float k1 = 1.0f, k2 = 1.0f;   // Asym: the two halves' speeds; PWM: k1 = 1 / width
    float factor = 1.0f;    // the steepest speed-up (for the mip level)
};

constexpr float kWarpTop = 0.99999994f;   // the largest float below 1: q never wraps to 0

// The amount whose speed-up is `factor` (the inverse of makeWarp's factor; 1 = no limit applies).
inline float warpAmountFor(int mode, float factor) {
    switch (mode) {
        case WP_SYNC: return (factor - 1.0f) / 15.0f;
        case WP_BEND_PLUS:
        case WP_BEND_MINUS: return (factor - 1.0f) / 3.0f;
        case WP_PWM: return (1.0f - 1.0f / factor) / 0.95f;
        case WP_ASYM_PLUS:
        case WP_ASYM_MINUS: return (0.5f - 0.5f / factor) / 0.45f;
        default: return 1.0f;
    }
}

Warp makeWarp(int mode, float amt, float maxFactor);

// maxFactor: the steepest speed-up the note allows (Nyquist over the oscillator's frequency, see
// renderOsc). A high note gets a gentler warp instead of one that folds back as noise: the
// squeezed part of a cycle can't be band-limited by the table's mip levels alone.
inline Warp makeWarp(int mode, float amt) { return makeWarp(mode, amt, 1e9f); }
inline Warp makeWarp(int mode, float amt, float maxFactor) {
    Warp w;
    w.mode = (mode > WP_OFF && mode < WP_COUNT) ? mode : WP_OFF;
    w.amt = std::min(std::max(amt, 0.0f), 1.0f);
    const float a = w.amt;
    switch (w.mode) {
        case WP_SYNC:
            w.r = 1.0f + 15.0f * a;
            w.factor = w.r;
            break;
        case WP_BEND_PLUS:
        case WP_BEND_MINUS:
            w.c = w.mode == WP_BEND_PLUS ? 1.0f / (1.0f + 3.0f * a) : 1.0f + 3.0f * a;
            w.factor = 1.0f + 3.0f * a;   // the curve's slope at its steepest end: 1/c or c
            break;
        case WP_PWM:
            w.k1 = 1.0f / (1.0f - 0.95f * a);
            w.factor = w.k1;
            break;
        case WP_ASYM_PLUS:
        case WP_ASYM_MINUS:
            w.m = w.mode == WP_ASYM_PLUS ? 0.5f - 0.45f * a : 0.5f + 0.45f * a;
            w.k1 = 0.5f / w.m;
            w.k2 = 0.5f / (1.0f - w.m);
            w.factor = std::max(w.k1, w.k2);
            break;
        case WP_MIRROR:
            w.factor = a > 0.0f ? 2.0f : 1.0f;   // the mirrored read runs the frame twice per cycle
            break;
        case WP_QUANTIZE:
            w.r = std::max(4.0f, std::floor(256.0f * std::exp2(-6.0f * a)));
            w.invR = 1.0f / w.r;
            break;
        default: break;
    }
    if (w.factor > maxFactor && w.mode != WP_MIRROR && w.mode != WP_QUANTIZE) {
        const float lower = std::min(std::max(warpAmountFor(w.mode, std::max(maxFactor, 1.0f)), 0.0f), w.amt);
        if (lower < w.amt) return makeWarp(w.mode, lower, 1e9f);
    }
    return w;
}

typedef int32_t i4w __attribute__((vector_size(16)));

inline f4 sel4(i4w mask, f4 a, f4 b) {   // mask lanes all ones: a, else b
    return (f4)(((i4w)a & mask) | ((i4w)b & ~mask));
}
inline f4 floorPos4(f4 x) {   // x >= 0
    return __builtin_convertvector(__builtin_convertvector(x, i4w), f4);
}
// The mirrored read: the frame forward in the first half of the cycle, backward in the second.
inline f4 mirror4(f4 p) {
    const i4w lo = p < splat(0.5f);
    return min4(sel4(lo, p + p, splat(2.0f) - p - p), splat(kWarpTop));
}

// The warped read position for four phases p (0..1). Mirror returns p itself: its second read
// is mirror4(p), blended by the amount (warpBlend).
template <int Mode>
inline f4 warp4(const Warp& w, f4 p) {
    f4 q = p;
    if (Mode == WP_SYNC) {
        const f4 x = p * splat(w.r);
        q = x - floorPos4(x);
    } else if (Mode == WP_BEND_PLUS || Mode == WP_BEND_MINUS) {
        const i4w lo = p < splat(0.5f);
        const f4 t = sel4(lo, p + p, splat(2.0f) - p - p);   // 0..1..0 over the cycle
        const f4 y = t * recip4(splat(w.c) + t * splat(1.0f - w.c));
        q = sel4(lo, splat(0.5f) * y, splat(1.0f) - splat(0.5f) * y);
    } else if (Mode == WP_PWM) {
        q = p * splat(w.k1);
    } else if (Mode == WP_ASYM_PLUS || Mode == WP_ASYM_MINUS) {
        const i4w lo = p < splat(w.m);
        q = sel4(lo, p * splat(w.k1), splat(0.5f) + (p - splat(w.m)) * splat(w.k2));
    } else if (Mode == WP_QUANTIZE) {
        q = floorPos4(p * splat(w.r)) * splat(w.invR);
    }
    return min4(max4(q, splat(0.0f)), splat(kWarpTop));
}

inline f4 warpAny4(const Warp& w, f4 p) {
    switch (w.mode) {
        case WP_SYNC: return warp4<WP_SYNC>(w, p);
        case WP_BEND_PLUS: return warp4<WP_BEND_PLUS>(w, p);
        case WP_BEND_MINUS: return warp4<WP_BEND_MINUS>(w, p);
        case WP_PWM: return warp4<WP_PWM>(w, p);
        case WP_ASYM_PLUS: return warp4<WP_ASYM_PLUS>(w, p);
        case WP_ASYM_MINUS: return warp4<WP_ASYM_MINUS>(w, p);
        case WP_QUANTIZE: return warp4<WP_QUANTIZE>(w, p);
        default: return min4(max4(p, splat(0.0f)), splat(kWarpTop));
    }
}

// One phase, for the wave view and the tests: the same code the oscillator runs.
inline float warpPhase(const Warp& w, float p) {
    float out[4];
    store4(out, warpAny4(w, splat(p)));
    return out[0];
}
inline float mirrorPhase(float p) {
    float out[4];
    store4(out, mirror4(splat(p)));
    return out[0];
}

} // namespace pf
