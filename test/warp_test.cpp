// Oscillator warp (dsp/warp.h): the phase maps, the engine paths (off stays bit-exact, every mode
// plays, finite and band-limited), the matrix targets, saved state and the wave view.
#include "host.h"
#include "../dsp/synth.h"
#include "../dsp/warp.h"

#include <cmath>
#include <vector>

namespace pft {
namespace {

pf::Patch base() {
    pf::Patch p;
    p.engine = pf::EN_CLEAN;   // no drift: exact pitch for the spectrum checks
    p.osc[0].unison = 1;
    p.osc[0].level = 0.8f;
    p.osc[0].wave = pf::OW_SAW;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    return p;
}

std::vector<float> take(const pf::Patch& p, int note, int samples, int skip = 40) {
    pf::Synth s(44100.0f);
    s.setPatch(p);
    s.noteOn(note, 127);
    float l[kBlock], r[kBlock];
    for (int b = 0; b < skip; ++b) s.render(l, r, kBlock);
    std::vector<float> out;
    while (static_cast<int>(out.size()) < samples) {
        s.render(l, r, kBlock);
        out.insert(out.end(), l, l + kBlock);
    }
    out.resize(static_cast<size_t>(samples));
    return out;
}

// The share of the signal's power (DC removed) at the note's harmonics (1 s at 1 Hz bins: harmonics of an integer
// frequency land on bins exactly). Aliases fall between them.
float harmonicShare(const std::vector<float>& xin, double f0) {
    const size_t n = xin.size();
    double mean = 0.0;   // PWM and Asym hold or linger on one side of the frame: a DC offset, not an alias
    for (float v : xin) mean += v;
    mean /= static_cast<double>(n);
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = xin[i] - mean;
    double total = 0.0;
    for (double v : x) total += v * v;
    double h = 0.0;
    for (int k = 1; k * f0 < 22050.0; ++k) {
        const double w = 2.0 * M_PI * k * f0 / 44100.0;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; ++i) {
            re += x[i] * std::cos(w * static_cast<double>(i));
            im -= x[i] * std::sin(w * static_cast<double>(i));
        }
        h += 2.0 * (re * re + im * im) / static_cast<double>(n);
    }
    return total > 0.0 ? static_cast<float>(h / total) : 0.0f;
}

double diff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
    return d;
}

void phaseMaps() {
    for (int mode = pf::WP_SYNC; mode < pf::WP_COUNT; ++mode)
        for (float a : {0.25f, 0.5f, 1.0f}) {
            const pf::Warp w = pf::makeWarp(mode, a);
            bool inRange = true, monotonic = true;
            float last = -1.0f;
            for (int i = 0; i < 4096; ++i) {
                const float p = static_cast<float>(i) / 4096.0f;
                const float q = mode == pf::WP_MIRROR ? pf::mirrorPhase(p) : pf::warpPhase(w, p);
                inRange = inRange && q >= 0.0f && q < 1.0f;
                if (mode != pf::WP_SYNC && mode != pf::WP_MIRROR) monotonic = monotonic && q >= last - 1e-6f;
                last = q;
            }
            CHECK(inRange);
            CHECK(monotonic);
            CHECK(w.factor >= 1.0f && w.factor <= 20.5f);
        }
    // Spot values: each map does what its name says.
    CHECK(std::fabs(pf::warpPhase(pf::makeWarp(pf::WP_SYNC, 1.0f), 0.1f) - 0.6f) < 1e-5f);   // 16 x 0.1 = 1.6
    CHECK(std::fabs(pf::warpPhase(pf::makeWarp(pf::WP_BEND_PLUS, 1.0f), 0.5f) - 0.5f) < 1e-4f);   // the middle stays
    CHECK(pf::warpPhase(pf::makeWarp(pf::WP_BEND_PLUS, 1.0f), 0.1f) > 0.1f);    // the start runs ahead
    CHECK(pf::warpPhase(pf::makeWarp(pf::WP_BEND_MINUS, 1.0f), 0.1f) < 0.1f);   // the start lags
    CHECK(pf::warpPhase(pf::makeWarp(pf::WP_PWM, 1.0f), 0.5f) > 0.999f);        // the frame is over at 5%: holds
    CHECK(std::fabs(pf::warpPhase(pf::makeWarp(pf::WP_ASYM_PLUS, 1.0f), 0.05f) - 0.5f) < 1e-4f);   // the middle at 5%
    CHECK(std::fabs(pf::warpPhase(pf::makeWarp(pf::WP_ASYM_MINUS, 1.0f), 0.95f) - 0.5f) < 1e-4f);
    CHECK(std::fabs(pf::mirrorPhase(0.25f) - 0.5f) < 1e-6f && std::fabs(pf::mirrorPhase(0.75f) - 0.5f) < 1e-6f);
    const float q = pf::warpPhase(pf::makeWarp(pf::WP_QUANTIZE, 1.0f), 0.3f);
    CHECK(std::fabs(q - 0.25f) < 1e-6f);   // 4 steps
    // A high note's limit: the warp gets gentler, never steeper than allowed.
    for (int mode : {pf::WP_SYNC, pf::WP_BEND_PLUS, pf::WP_PWM, pf::WP_ASYM_MINUS}) {
        const pf::Warp w = pf::makeWarp(mode, 1.0f, 3.0f);
        CHECK(w.factor <= 3.001f && w.factor > 2.95f && w.amt < 1.0f);
        CHECK(pf::makeWarp(mode, 0.05f, 3.0f).amt == 0.05f);   // below the limit: untouched
    }
}

void enginePaths() {
    pf::Patch off = base();
    const std::vector<float> ref = take(off, 60, 4096);
    // A mode with amount 0, or the amount with the mode off: the plain path, bit for bit.
    pf::Patch zero = base();
    zero.osc[0].warp = pf::WP_SYNC;
    CHECK(diff(take(zero, 60, 4096), ref) == 0.0);
    pf::Patch noMode = base();
    noMode.osc[0].warpAmt = 1.0f;
    CHECK(diff(take(noMode, 60, 4096), ref) == 0.0);

    // Every mode at full amount: something else, finite, not louder than a few times the plain saw.
    for (int mode = pf::WP_SYNC; mode < pf::WP_COUNT; ++mode) {
        pf::Patch p = base();
        p.osc[0].warp = mode;
        p.osc[0].warpAmt = 1.0f;
        const std::vector<float> x = take(p, 60, 4096);
        bool finite = true;
        float peak = 0.0f;
        for (float v : x) {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, std::fabs(v));
        }
        if (!(finite && peak < 3.0f && diff(x, ref) > 0.05)) std::printf("  warp mode %d: peak %g\n", mode, peak);
        CHECK(finite && peak < 3.0f && peak > 0.05f);
        CHECK(diff(x, ref) > 0.05);
    }

    // The same warp on every play path: the frame cache (float), 16-bit, two frames morphed. A wavetable
    // position between frames, held still (cache) or moving (morph), plays the same notes in tune.
    for (int mode : {pf::WP_BEND_MINUS, pf::WP_MIRROR}) {
        pf::Patch p = base();
        p.osc[0].wave = pf::OW_TABLE;
        p.osc[0].pos = 0.5f;
        p.osc[0].warp = mode;
        p.osc[0].warpAmt = 0.7f;
        pf::Synth cached(44100.0f), plain(44100.0f);
        plain.setFrameCache(false);
        for (pf::Synth* s : {&cached, &plain}) {
            s->setPatch(p);
            s->noteOn(57, 127);
        }
        float l1[kBlock], r1[kBlock], l2[kBlock], r2[kBlock];
        double d = 0.0;
        for (int b = 0; b < 60; ++b) {
            cached.render(l1, r1, kBlock);
            plain.render(l2, r2, kBlock);
            for (int i = 0; i < kBlock; ++i) d = std::max(d, static_cast<double>(std::fabs(l1[i] - l2[i])));
        }
        CHECK(d < 1e-3);
    }

    // Band-limited: the continuous warps keep a high note's power on its harmonics (A5, 880 Hz; the squeezed
    // parts can't be band-limited by the tables alone, so makeWarp's limit eases a warp on high notes).
    for (int mode : {pf::WP_BEND_PLUS, pf::WP_BEND_MINUS, pf::WP_PWM, pf::WP_ASYM_PLUS, pf::WP_ASYM_MINUS, pf::WP_MIRROR}) {
        pf::Patch p = base();
        p.osc[0].warp = mode;
        p.osc[0].warpAmt = 1.0f;
        const float share = harmonicShare(take(p, 81, 44100), 880.0);
        if (share < 0.98f) std::printf("  warp mode %d: %.4f of the power on the harmonics\n", mode, share);
        CHECK(share >= 0.98f);
    }
    const float plainShare = harmonicShare(take(base(), 81, 44100), 880.0);
    CHECK(plainShare >= 0.995f);

    // Sync raises the brightness with the amount.
    auto crossings = [](const std::vector<float>& x) {
        int n = 0;
        for (size_t i = 1; i < x.size(); ++i) n += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
        return n;
    };
    pf::Patch s1 = base(), s2 = base();
    s1.osc[0].wave = s2.osc[0].wave = pf::OW_SINE;
    s1.osc[0].warp = s2.osc[0].warp = pf::WP_SYNC;
    s1.osc[0].warpAmt = 0.2f;
    s2.osc[0].warpAmt = 0.8f;
    CHECK(crossings(take(s2, 48, 8192)) > 2 * crossings(take(s1, 48, 8192)));
}

void pluginSide() {
    // Matrix: Constant -> Osc1 Warp opens a warp whose knob is at 0.
    Host a, b;
    for (Host* h : {&a, &b}) {
        h->bare();
        h->set(pf::P_O1_WAVE, pf::OW_SAW);
        h->set(pf::P_ENGINE, pf::EN_CLEAN);
        h->set(pf::P_O1_WARP, pf::WP_PWM);
    }
    b.set(pf::P_M1_SRC, pf::MS_CONSTANT);
    b.set(pf::P_M1_T1, pf::MT_O1_WARP);
    b.set(pf::P_M1_A1, 0.8f);
    CHECK(b.display(pf::P_M1_T1) == "Osc1 Warp");
    a.on(60, 100);
    b.on(60, 100);
    a.run(40);
    b.run(40);
    double d = 0.0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    CHECK(d > 0.05);

    // Names, display and saved state.
    Host h;
    CHECK(h.name(pf::P_O1_WARP) == "O1 Warp" && h.name(pf::P_O2_WARP_AMT) == "O2 Warp Amt");
    CHECK(h.display(pf::P_O1_WARP) == "Off");
    h.set(pf::P_O1_WARP, pf::WP_ASYM_MINUS);
    h.set(pf::P_O1_WARP_AMT, 0.4f);
    h.set(pf::P_O2_WARP, pf::WP_QUANTIZE);
    CHECK(h.display(pf::P_O1_WARP) == "Asym-" && h.display(pf::P_O1_WARP_AMT) == "40%");
    const std::string state = h.chunk();
    CHECK(state.find("o1_warp=6\n") != std::string::npos && state.find("o1_warp_amt=0.4\n") != std::string::npos);
    Host c;
    CHECK(c.load(state) == 1);
    CHECK(c.value(pf::P_O1_WARP) == pf::WP_ASYM_MINUS && c.display(pf::P_O1_WARP_AMT) == "40%" &&
          c.value(pf::P_O2_WARP) == pf::WP_QUANTIZE);
    // A preset from before the warp: off.
    Host old;
    CHECK(old.load("polyforce 4\nvolume=-6\n") == 1);
    CHECK(old.value(pf::P_O1_WARP) == pf::WP_OFF);

    // The wave view follows the warp.
    Host v;
    v.set(pf::P_O1_WAVE, pf::OW_SAW);
    v.run(40);
    std::vector<float> before;
    for (int c2 = 0; c2 < pf::kWaveCols; ++c2) before.push_back(v.get(pf::P_O1_WV01 + c2));
    v.set(pf::P_O1_WARP, pf::WP_PWM);
    v.set(pf::P_O1_WARP_AMT, 1.0f);
    v.run(40);
    int changed = 0;
    for (int c2 = 0; c2 < pf::kWaveCols; ++c2) changed += v.get(pf::P_O1_WV01 + c2) != before[static_cast<size_t>(c2)];
    CHECK(changed > pf::kWaveCols / 2);
}

} // namespace

void warpTests() {
    phaseMaps();
    enginePaths();
    pluginSide();
}

} // namespace pft
