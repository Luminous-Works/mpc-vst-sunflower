// Lumina FX: EQ. The third insert, to the same bar as the compressor and saturator (HANDOFF.md section 4).
// Header-only C++17, no dependencies, no allocation anywhere; MPC-safe.
//
// BANDS (signal order): HIGH-PASS (2nd or 4th order Butterworth) -> LOW SHELF -> BELL 1 -> BELL 2 -> HIGH SHELF -> LOW-PASS (2nd or 4th order).
// Every band is an exact biquad from the standard bilinear-transform designs (R. Bristow-Johnson's cookbook) with the frequency pre-warped:
//   bell       H(s) = (s^2 + s A/Q + 1) / (s^2 + s /(A Q) + 1),            A = 10^(dB/40): gain at the centre is EXACTLY the set dB,
//                                                                          and a cut is the exact inverse of the same boost
//   low shelf  H(s) = A (s^2 + sqrt(A)/Q' s + A) / (A s^2 + sqrt(A)/Q' s + 1)   (shelf slope S = 1: gain at the corner is exactly dB/2)
//   high shelf mirrored; high-pass / low-pass 1/(s^2 + s/Q + 1) with Butterworth Q (0.7071, or 0.5412 and 1.3066 for 4th order): -3.01 dB at the cutoff.
// All arithmetic and all state are double (transposed direct form II). Controls are SMOOTHED in the log-frequency / dB / log-Q domain (20 ms),
// with the coefficients refreshed every 16 samples while a control is moving and not at all when everything has settled, so sweeping never
// clicks and a static EQ costs only its biquads. A band whose gain is 0 dB is skipped entirely (zero cost, bit-exact), and with every band off
// the buffer is not touched. Latency is 0. Non-finite input becomes 0; state is flushed below 1e-30.
//
// eqResponseDb() evaluates the magnitude response of a setting from the same coefficient code: the UI draws its curve with a JS mirror of the
// same formulas, and tests/test_eq.cpp checks the maths independently (bell and shelf gains, Butterworth -3.01 dB, boost/cut cancellation,
// the process loop against the curve at 20 frequencies, click-free sweeps, stability, sample-rate behaviour).
//
// Known limit of any bilinear-transform design (stated, and measured in the tests): a bell or shelf placed near Nyquist is narrower on its
// upper side than the analogue prototype (frequency cramping); the centre gain stays exact.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace lumina::fx {

struct EqParams {
    bool  hpOn = false;  float hpHz = 80.0f;     bool hp24 = false;   // high-pass, 20 .. 500 Hz
    float lsHz = 120.0f; float lsDb = 0.0f;                             // low shelf, 40 .. 500 Hz, +-18 dB
    float b1Hz = 400.0f; float b1Db = 0.0f; float b1Q = 1.0f;          // bell 1, 60 .. 2000 Hz
    float b2Hz = 3000.0f; float b2Db = 0.0f; float b2Q = 1.0f;         // bell 2, 500 .. 12000 Hz
    float hsHz = 8000.0f; float hsDb = 0.0f;                            // high shelf, 2000 .. 16000 Hz
    bool  lpOn = false;  float lpHz = 18000.0f; bool lp24 = false;     // low-pass, 2000 .. 20000 Hz
    float outDb = 0.0f;                                                  // output trim, +-24 dB
    bool  bypass = false;
};

namespace eqdetail {
struct Coef { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
constexpr double kPi = 3.14159265358979323846;

inline Coef norm(double b0, double b1, double b2, double a0, double a1, double a2) { return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 }; }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double maxHz(double sr) { return 0.45 * sr; }

inline Coef bell(double sr, double f, double dB, double Q) {
    const double A = std::pow(10.0, dB / 40.0), w = 2.0 * kPi * clampd(f, 10.0, maxHz(sr)) / sr, c = std::cos(w), al = std::sin(w) / (2.0 * clampd(Q, 0.1, 20.0));
    return norm(1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
}
inline Coef lowShelf(double sr, double f, double dB) {
    const double A = std::pow(10.0, dB / 40.0), w = 2.0 * kPi * clampd(f, 10.0, maxHz(sr)) / sr, c = std::cos(w), s2a = 2.0 * std::sqrt(A) * (std::sin(w) / 1.41421356237309505);
    return norm(A * ((A + 1) - (A - 1) * c + s2a), 2.0 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s2a),
                (A + 1) + (A - 1) * c + s2a, -2.0 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s2a);
}
inline Coef highShelf(double sr, double f, double dB) {
    const double A = std::pow(10.0, dB / 40.0), w = 2.0 * kPi * clampd(f, 10.0, maxHz(sr)) / sr, c = std::cos(w), s2a = 2.0 * std::sqrt(A) * (std::sin(w) / 1.41421356237309505);
    return norm(A * ((A + 1) + (A - 1) * c + s2a), -2.0 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s2a),
                (A + 1) - (A - 1) * c + s2a, 2.0 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s2a);
}
inline Coef highPass(double sr, double f, double Q) {
    const double w = 2.0 * kPi * clampd(f, 10.0, maxHz(sr)) / sr, c = std::cos(w), al = std::sin(w) / (2.0 * Q);
    return norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
inline Coef lowPass(double sr, double f, double Q) {
    const double w = 2.0 * kPi * clampd(f, 10.0, maxHz(sr)) / sr, c = std::cos(w), al = std::sin(w) / (2.0 * Q);
    return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
constexpr double kQ2 = 0.70710678118654752, kQ4a = 0.54119610014619698, kQ4b = 1.3065629648763766;   // Butterworth: 2nd order; 4th order sections

/// The sections of a setting in signal order (at most 2 + 1 + 1 + 1 + 1 + 2): which bands are active and their coefficients.
struct Section { Coef c; int slot; double wet; };   // slot: a band's FIXED place in the state array (HP 0,1; LS 2; B1 3; B2 4; HS 5; LP 6,7); wet: 1 = fully in
struct Chain {
    Section s[8]; int n = 0; unsigned mask = 0;   // mask: which slots are in use
    void add(const Coef& c, int slot, double wet = 1.0) { s[n].c = c; s[n].slot = slot; s[n].wet = wet; ++n; mask |= 1u << slot; }
};
/// hpWet / lpWet: how far the high-pass / low-pass are crossfaded in (1 = fully; only < 1 while one is being switched on or off).
inline Chain design(const EqParams& p, double sr, double hpWet = 1.0, double lpWet = 1.0) {
    Chain ch;
    if (p.hpOn) { if (p.hp24) { ch.add(highPass(sr, p.hpHz, kQ4a), 0, hpWet); ch.add(highPass(sr, p.hpHz, kQ4b), 1, hpWet); } else ch.add(highPass(sr, p.hpHz, kQ2), 0, hpWet); }
    if (std::fabs(p.lsDb) > 1e-4f) ch.add(lowShelf(sr, p.lsHz, p.lsDb), 2);
    if (std::fabs(p.b1Db) > 1e-4f) ch.add(bell(sr, p.b1Hz, p.b1Db, p.b1Q), 3);
    if (std::fabs(p.b2Db) > 1e-4f) ch.add(bell(sr, p.b2Hz, p.b2Db, p.b2Q), 4);
    if (std::fabs(p.hsDb) > 1e-4f) ch.add(highShelf(sr, p.hsHz, p.hsDb), 5);
    if (p.lpOn) { if (p.lp24) { ch.add(lowPass(sr, p.lpHz, kQ4a), 6, lpWet); ch.add(lowPass(sr, p.lpHz, kQ4b), 7, lpWet); } else ch.add(lowPass(sr, p.lpHz, kQ2), 6, lpWet); }
    return ch;
}
inline double magnitude(const Coef& c, double w) {
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}
} // namespace eqdetail

/// Magnitude response of a setting in dB at frequency f (Hz) for sample rate sr: the same coefficients the process loop uses (output trim included).
inline double eqResponseDb(const EqParams& p, double sr, double f) {
    const auto ch = eqdetail::design(p, sr);
    const double w = 2.0 * eqdetail::kPi * f / sr;
    double mag = 1.0;
    for (int i = 0; i < ch.n; ++i) mag *= eqdetail::magnitude(ch.s[i].c, w);
    return 20.0 * std::log10(std::max(mag, 1e-30)) + static_cast<double>(p.outDb);
}

class Eq {
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate, int channels = 2) {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        ch_ = std::min(std::max(channels, 1), kMaxChannels);
        stepC_ = 1.0 - std::exp(-static_cast<double>(kStep) / (0.020 * sr_));
        wetC_ = 1.0 - std::exp(-1.0 / (0.010 * sr_));
        setParams(p_);
        snap();
        reset();
    }

    void reset() { for (auto& c : z_) for (auto& s : c) s = Z{}; }

    /// Allocation free. The change is smoothed: audio keeps playing through a sweep.
    void setParams(const EqParams& in) {
        p_ = in;
        p_.hpHz = cl(p_.hpHz, 20.0f, 500.0f); p_.lsHz = cl(p_.lsHz, 40.0f, 500.0f); p_.lsDb = cl(p_.lsDb, -18.0f, 18.0f);
        p_.b1Hz = cl(p_.b1Hz, 60.0f, 2000.0f); p_.b1Db = cl(p_.b1Db, -18.0f, 18.0f); p_.b1Q = cl(p_.b1Q, 0.2f, 10.0f);
        p_.b2Hz = cl(p_.b2Hz, 500.0f, 12000.0f); p_.b2Db = cl(p_.b2Db, -18.0f, 18.0f); p_.b2Q = cl(p_.b2Q, 0.2f, 10.0f);
        p_.hsHz = cl(p_.hsHz, 2000.0f, 16000.0f); p_.hsDb = cl(p_.hsDb, -18.0f, 18.0f);
        p_.lpHz = cl(p_.lpHz, 2000.0f, 20000.0f); p_.outDb = cl(p_.outDb, -24.0f, 24.0f);
        // targets in the domains the smoothing runs in: log frequency, dB, log Q
        t_[0] = std::log(static_cast<double>(p_.hpHz));
        t_[1] = std::log(static_cast<double>(p_.lsHz)); t_[2] = p_.lsDb;
        t_[3] = std::log(static_cast<double>(p_.b1Hz)); t_[4] = p_.b1Db; t_[5] = std::log(static_cast<double>(p_.b1Q));
        t_[6] = std::log(static_cast<double>(p_.b2Hz)); t_[7] = p_.b2Db; t_[8] = std::log(static_cast<double>(p_.b2Q));
        t_[9] = std::log(static_cast<double>(p_.hsHz)); t_[10] = p_.hsDb; t_[11] = std::log(static_cast<double>(p_.lpHz)); t_[12] = p_.outDb;
        hpT_ = p_.hpOn ? 1.0 : 0.0; lpT_ = p_.lpOn ? 1.0 : 0.0;   // switching the high-pass / low-pass crossfades it PER SAMPLE (a bass note feels even a 10 Hz high-pass: ~14 degrees of phase)
        moving_ = true;
    }

    /// Jump the smoothed controls to their targets (used by prepare; useful for presets and tests).
    void snap() { for (int i = 0; i < kN; ++i) s_[i] = t_[i]; hpWet_ = hpT_; lpWet_ = lpT_; refresh(); moving_ = false; }

    const EqParams& params() const { return p_; }
    int latencySamples() const { return 0; }
    /// True when every band is off or flat and the trim is 0 dB (the buffer is passed through untouched).
    bool isFlat() const { return chain_.n == 0 && std::fabs(outDbS_) < 1e-6 && !p_.hpOn && !p_.lpOn; }

    void process(float* const* io, int channels, int n) {
        if (p_.bypass || n <= 0) return;
        const int C = std::min(channels, ch_);
        int i = 0;
        while (i < n) {
            if (until_ == 0) {   // one step every 16 SAMPLES, whatever the block size
                if (moving_) advance();
                else if ((p_.hpOn || hpWet_ > 1e-4) != hpIn_ || (p_.lpOn || lpWet_ > 1e-4) != lpIn_) refresh();
                until_ = kStep;
            }
            const int m = std::min(until_, n - i);
            until_ -= m;
            if (chain_.n == 0 && std::fabs(outDbS_) < 1e-6 && !moving_) { i += m; continue; }   // flat: bit exact, no work
            double wh[kStep], wl[kStep];   // the crossfade of the high-pass / low-pass, one value per sample (a staircase would step the output)
            for (int k = 0; k < m; ++k) {
                hpWet_ += (hpT_ - hpWet_) * wetC_; lpWet_ += (lpT_ - lpWet_) * wetC_;
                if (std::fabs(hpT_ - hpWet_) < 1e-9) hpWet_ = hpT_;
                if (std::fabs(lpT_ - lpWet_) < 1e-9) lpWet_ = lpT_;
                wh[k] = hpWet_; wl[k] = lpWet_;
            }
            // (a section whose crossfade has arrived at 0 or just left 0 is added / removed at the next 16-sample boundary, above)
            for (int c = 0; c < C; ++c) {
                float* x = io[c] + i;
                for (int k = 0; k < m; ++k) {
                    double v = static_cast<double>(x[k]);
                    if (!std::isfinite(v)) v = 0.0;
                    for (int s = 0; s < chain_.n; ++s) {
                        const eqdetail::Coef& q = chain_.s[s].c; Z& z = z_[c][chain_.s[s].slot];
                        const double y = q.b0 * v + z.z1;
                        z.z1 = q.b1 * v - q.a1 * y + z.z2; z.z2 = q.b2 * v - q.a2 * y;
                        if (std::fabs(z.z1) < 1e-30 && std::fabs(z.z2) < 1e-30) z.z1 = z.z2 = 0.0;   // together, or a resonator near the threshold never dies
                        const int sl = chain_.s[s].slot;
                        const double w = sl <= 1 ? wh[k] : (sl >= 6 ? wl[k] : 1.0);
                        v = (w >= 1.0) ? y : v + w * (y - v);
                    }
                    x[k] = static_cast<float>(v * gain_);
                }
            }
            i += m;
        }
    }

private:
    static constexpr int kStep = 16, kN = 13;
    struct Z { double z1 = 0, z2 = 0; };
    static float cl(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // advance the smoothed controls one step; refresh the coefficients; stop when everything has arrived
    void advance() {
        bool any = false;
        for (int i = 0; i < kN; ++i) {
            const double d = t_[i] - s_[i];
            if (std::fabs(d) < 1e-7) { s_[i] = t_[i]; continue; }
            s_[i] += d * stepC_; any = true;
        }
        refresh();
        moving_ = any;
    }
    void refresh() {
        EqParams q = p_;   // the smoothed values, in natural units
        q.hpHz = static_cast<float>(std::exp(s_[0])); q.lsHz = static_cast<float>(std::exp(s_[1])); q.lsDb = static_cast<float>(s_[2]);
        q.b1Hz = static_cast<float>(std::exp(s_[3])); q.b1Db = static_cast<float>(s_[4]); q.b1Q = static_cast<float>(std::exp(s_[5]));
        q.b2Hz = static_cast<float>(std::exp(s_[6])); q.b2Db = static_cast<float>(s_[7]); q.b2Q = static_cast<float>(std::exp(s_[8]));
        q.hsHz = static_cast<float>(std::exp(s_[9])); q.hsDb = static_cast<float>(s_[10]); q.lpHz = static_cast<float>(std::exp(s_[11]));
        q.hpOn = p_.hpOn || hpWet_ > 1e-4;   // still crossfading out? keep the section until it is gone
        q.lpOn = p_.lpOn || lpWet_ > 1e-4;
        hpIn_ = q.hpOn; lpIn_ = q.lpOn;
        outDbS_ = s_[12];
        // a band that was not in use starts from clean state; a band that stays keeps its own state whatever else comes and goes
        const eqdetail::Chain next = eqdetail::design(q, sr_);
        const unsigned changed = next.mask ^ chain_.mask;
        for (int slot = 0; slot < 8; ++slot) if (changed & (1u << slot)) for (auto& c : z_) c[slot] = Z{};
        chain_ = next;
        gain_ = std::pow(10.0, outDbS_ / 20.0);
    }

    EqParams p_{};
    double sr_ = 48000.0, stepC_ = 0.01, t_[kN] = {}, s_[kN] = {}, outDbS_ = 0.0, gain_ = 1.0;
    int ch_ = 2, until_ = 0;
    bool moving_ = false, hpIn_ = false, lpIn_ = false;
    double hpT_ = 0.0, lpT_ = 0.0, hpWet_ = 0.0, lpWet_ = 0.0, wetC_ = 0.01;
    eqdetail::Chain chain_{};
    Z z_[kMaxChannels][8];
};

} // namespace lumina::fx
