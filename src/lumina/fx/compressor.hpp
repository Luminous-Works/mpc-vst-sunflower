// Lumina FX: Compressor. The reference insert, written to the quality bar in HANDOFF.md section 4.
// Header-only C++17, no dependencies, no allocation after prepare(); safe to share with the MPC plugins (cross-compiles for ARM).
//
// SIGNAL FLOW (per sample)
//   in -> sidechain high-pass (2-pole Butterworth, so bass does not pump the mix)
//      -> level detector: peak envelope (fast release) blended with an RMS envelope (rmsBlend 0 = peak .. 1 = RMS;
//         the RMS reading is scaled by sqrt(2) so a sine reads the same in both: level = amplitude)
//      -> channel link: the louder channel drives both (no image shift)
//      -> dB -> gain computer with a quadratic soft knee (Giannoulis, Massberg, Reiss, JAES 2012):
//             over = x - T,   slope = 1 - 1/R
//             GR = 0                                   for 2*over < -W
//             GR = slope * (over + W/2)^2 / (2*W)      for |2*over| <= W        (W = knee width, dB)
//             GR = slope * over                        for 2*over > W
//      -> attack / release smoothing of the gain reduction in the dB domain (one-pole, time constants in ms, exact
//         exp() coefficients so the times are sample-rate independent)
//      -> AUTO RELEASE (optional): the release time stretches up to 4x while the *average* gain reduction (500 ms) is deep,
//         so a short hit recovers quickly and sustained heavy compression does not pump
//      -> gain applied to the (optionally look-ahead delayed) audio, makeup, dry/wet mix, output trim.
//   Bypass is bit exact (the buffer is not touched). Non-finite input is replaced by 0 before it can poison the state.
//   Denormals: all recursive state is double and flushed below 1e-30.
//
// MEASURED, not assumed: tests/test_compressor.cpp checks the static curve against the formula above, attack and release
// times, sample-rate independence, bit-exact bypass, look-ahead latency, stability and low-order distortion.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lumina::fx {

struct CompParams {
    float thresholdDb = -18.0f;   // -60 .. 0
    float ratio       = 4.0f;     // 1 .. 100 (100 = limiting)
    float kneeDb      = 6.0f;     // 0 .. 24   (0 = hard knee)
    float attackMs    = 10.0f;    // 0.05 .. 200
    float releaseMs   = 120.0f;   // 5 .. 2000
    bool  autoRelease = true;
    float makeupDb    = 0.0f;     // -24 .. 24
    bool  autoMakeup  = false;    // makeup = -T * (1 - 1/R) / 2: half the gain reduction at 0 dBFS, so loudness stays close
    float mix         = 1.0f;     // 0 dry .. 1 wet (parallel compression)
    float sidechainHz = 80.0f;    // 0 = off, else 20 .. 500
    float rmsBlend    = 0.6f;     // 0 peak .. 1 RMS
    float lookaheadMs = 0.0f;     // 0 .. 5 (adds that much latency; latencySamples() reports it)
    float outputDb    = 0.0f;     // -24 .. 12
    bool  bypass      = false;
};

/// Gain reduction in dB (>= 0) for a static input level, the exact gain computer used by the process loop.
inline double staticGainReductionDb(const CompParams& p, double inDb) {
    const double W = std::max(0.0, static_cast<double>(p.kneeDb));
    const double slope = 1.0 - 1.0 / std::max(1.0, static_cast<double>(p.ratio));
    const double over = inDb - static_cast<double>(p.thresholdDb);
    if (slope <= 0.0) return 0.0;
    if (W <= 0.0) return over > 0.0 ? slope * over : 0.0;
    if (2.0 * over < -W) return 0.0;
    if (2.0 * std::fabs(over) <= W) return slope * (over + W / 2.0) * (over + W / 2.0) / (2.0 * W);
    return slope * over;
}

class Compressor {
public:
    static constexpr int kMaxChannels = 2;

    /// Allocates (once). Call from a non-audio thread.
    void prepare(double sampleRate, int channels = 2) {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        ch_ = std::min(std::max(channels, 1), kMaxChannels);
        const size_t maxLook = static_cast<size_t>(std::ceil(sr_ * 0.005)) + 2;
        for (auto& d : delay_) d.assign(maxLook, 0.0f);
        setParams(p_);
        reset();
    }

    void reset() {
        for (auto& d : delay_) std::fill(d.begin(), d.end(), 0.0f);
        wr_ = 0;
        for (auto& b : hp_) b = Biquad{};
        peakEnv_ = rmsMs_ = gr_ = avgGr_ = 0.0;
        relDyn_ = relCoef_; ctr_ = 0;
    }

    /// Cheap enough for the audio thread (a few exp() calls); allocation free.
    void setParams(const CompParams& in) {
        p_ = in;
        p_.thresholdDb = clampf(p_.thresholdDb, -60.0f, 0.0f);
        p_.ratio = clampf(p_.ratio, 1.0f, 100.0f);
        p_.kneeDb = clampf(p_.kneeDb, 0.0f, 24.0f);
        p_.attackMs = clampf(p_.attackMs, 0.05f, 200.0f);
        p_.releaseMs = clampf(p_.releaseMs, 5.0f, 2000.0f);
        p_.makeupDb = clampf(p_.makeupDb, -24.0f, 24.0f);
        p_.mix = clampf(p_.mix, 0.0f, 1.0f);
        p_.rmsBlend = clampf(p_.rmsBlend, 0.0f, 1.0f);
        p_.lookaheadMs = clampf(p_.lookaheadMs, 0.0f, 5.0f);
        p_.outputDb = clampf(p_.outputDb, -24.0f, 12.0f);
        atkCoef_ = std::exp(-1.0 / (static_cast<double>(p_.attackMs) * 1e-3 * sr_));
        relCoef_ = std::exp(-1.0 / (static_cast<double>(p_.releaseMs) * 1e-3 * sr_));
        // peak detector release: 10 ms, but never more than a tenth of the release time, or it would add to a short release setting
        // (measured: a fixed 10 ms made a 5 ms release behave like 22 ms)
        peakRel_ = std::exp(-1.0 / (std::min(0.010, 0.1 * static_cast<double>(p_.releaseMs) * 1e-3) * sr_));
        rmsCoef_ = std::exp(-1.0 / (0.030 * sr_));      // RMS window ~30 ms
        avgCoef_ = std::exp(-1.0 / (0.500 * sr_));      // average gain reduction for auto release, 500 ms
        look_ = static_cast<int>(std::lround(static_cast<double>(p_.lookaheadMs) * 1e-3 * sr_));
        if (!delay_[0].empty()) look_ = std::min(look_, static_cast<int>(delay_[0].size()) - 2);
        makeup_ = static_cast<double>(p_.makeupDb) + static_cast<double>(p_.outputDb)
                + (p_.autoMakeup ? -static_cast<double>(p_.thresholdDb) * (1.0 - 1.0 / static_cast<double>(p_.ratio)) * 0.5 : 0.0);
        if (p_.sidechainHz >= 20.0f) designHp(static_cast<double>(clampf(p_.sidechainHz, 20.0f, 500.0f)));
        else hpOn_ = false;
        if (!p_.autoRelease) relDyn_ = relCoef_;
    }

    const CompParams& params() const { return p_; }
    int latencySamples() const { return p_.bypass ? 0 : look_; }
    /// Current gain reduction in dB (>= 0), for a meter.
    float gainReductionDb() const { return static_cast<float>(gr_); }

    /// In place. io[c][0..n). channels must match prepare().
    void process(float* const* io, int channels, int n) {
        if (p_.bypass || n <= 0) return;
        const int C = std::min(channels, ch_);
        const double knee = static_cast<double>(p_.kneeDb);
        const double slope = 1.0 - 1.0 / static_cast<double>(p_.ratio);
        const double T = static_cast<double>(p_.thresholdDb);
        const double mix = static_cast<double>(p_.mix), rb = static_cast<double>(p_.rmsBlend);
        const size_t len = delay_[0].size();
        for (int i = 0; i < n; ++i) {
            // ---- detector on the undelayed signal ----
            double pk = 0.0, ms = 0.0;
            float x[kMaxChannels] = {0.0f, 0.0f};
            for (int c = 0; c < C; ++c) {
                float v = io[c][i];
                if (!std::isfinite(v)) v = 0.0f;
                x[c] = v;
                double s = static_cast<double>(v);
                if (hpOn_) s = hp_[c].tick(s, b0_, b1_, b2_, a1_, a2_);
                pk = std::max(pk, std::fabs(s));
                ms = std::max(ms, s * s);
            }
            peakEnv_ = std::max(pk, peakEnv_ * peakRel_); flush(peakEnv_);
            rmsMs_ = ms + (rmsMs_ - ms) * rmsCoef_; flush(rmsMs_);
            const double level = (1.0 - rb) * peakEnv_ + rb * std::sqrt(rmsMs_) * 1.41421356237309505;
            const double xdb = 20.0 * std::log10(std::max(level, 1e-9));
            // ---- gain computer (see header) ----
            double target = 0.0;
            if (slope > 0.0) {
                const double over = xdb - T;
                if (knee <= 0.0) target = over > 0.0 ? slope * over : 0.0;
                else if (2.0 * over < -knee) target = 0.0;
                else if (2.0 * std::fabs(over) <= knee) target = slope * (over + knee / 2.0) * (over + knee / 2.0) / (2.0 * knee);
                else target = slope * over;
            }
            // ---- attack / release in the dB domain ----
            if (target > gr_) gr_ = atkCoef_ * gr_ + (1.0 - atkCoef_) * target;
            else gr_ = relDyn_ * gr_ + (1.0 - relDyn_) * target;
            flush(gr_);
            if (p_.autoRelease) {
                avgGr_ = avgCoef_ * avgGr_ + (1.0 - avgCoef_) * gr_; flush(avgGr_);
                if ((++ctr_ & 15) == 0) {   // every 16 samples is plenty: the average moves over hundreds of ms
                    const double w = std::min(1.0, avgGr_ / 10.0);
                    relDyn_ = std::exp(-1.0 / (static_cast<double>(p_.releaseMs) * (1.0 + 3.0 * w) * 1e-3 * sr_));
                }
            }
            const double gain = std::exp((makeup_ - gr_) * 0.11512925464970229);   // 10^(dB/20)
            // ---- audio path, optionally delayed by the look-ahead ----
            for (int c = 0; c < C; ++c) {
                float dry = x[c];
                if (look_ > 0) {
                    auto& d = delay_[c];
                    d[wr_] = x[c];
                    dry = d[(wr_ + len - static_cast<size_t>(look_)) % len];
                }
                const float wet = static_cast<float>(static_cast<double>(dry) * gain);
                io[c][i] = mix >= 1.0 ? wet : static_cast<float>(static_cast<double>(dry) * (1.0 - mix) + static_cast<double>(wet) * mix);
            }
            if (look_ > 0) wr_ = (wr_ + 1) % len;
        }
    }

private:
    struct Biquad {
        double z1 = 0, z2 = 0;
        double tick(double x, double b0, double b1, double b2, double a1, double a2) {   // transposed direct form II
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            if (std::fabs(z1) < 1e-30 && std::fabs(z2) < 1e-30) z1 = z2 = 0.0;   // together: flushing them one by one can sustain a limit cycle near the threshold
            return y;
        }
    };
    static void flush(double& v) { if (std::fabs(v) < 1e-30) v = 0.0; }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    void designHp(double f) {   // RBJ 2-pole Butterworth high-pass (Q = 1/sqrt(2)), bilinear transform with prewarp
        const double w0 = 6.283185307179586 * std::min(f, sr_ * 0.45) / sr_, cw = std::cos(w0), al = std::sin(w0) / (2.0 * 0.70710678118654752);
        const double a0 = 1.0 + al;
        b0_ = (1.0 + cw) / 2.0 / a0; b1_ = -(1.0 + cw) / a0; b2_ = b0_;
        a1_ = -2.0 * cw / a0; a2_ = (1.0 - al) / a0;
        hpOn_ = true;
    }

    CompParams p_{};
    double sr_ = 48000.0;
    int ch_ = 2, look_ = 0;
    size_t wr_ = 0;
    std::vector<float> delay_[kMaxChannels];
    Biquad hp_[kMaxChannels];
    bool hpOn_ = false;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double atkCoef_ = 0, relCoef_ = 0, relDyn_ = 0, peakRel_ = 0, rmsCoef_ = 0, avgCoef_ = 0, makeup_ = 0;
    double peakEnv_ = 0, rmsMs_ = 0, gr_ = 0, avgGr_ = 0;
    unsigned ctr_ = 0;
};

} // namespace lumina::fx
