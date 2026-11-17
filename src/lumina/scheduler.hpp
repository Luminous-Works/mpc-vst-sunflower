// Lumina Composer engine: the scheduler. Host transport (PPQ, tempo, play state) in, sample-accurate MIDI events out.
//
// Called once per audio block from processBlock. It never allocates, never locks and never blocks: everything lives in
// fixed-size arrays. Each gate onset is a pure function of (voice, tick k), and it is emitted by whichever block contains
// it. Rendering a passage in one big block or in many small blocks therefore gives the same events (tests check this).
// That property is what makes a plugin stable under any host buffer size.
#pragma once
#include "fib_voice.hpp"
#include "voice.hpp"
#include <cmath>

namespace lumina {

struct MidiEvent {
    int sampleOffset;      // within the current block
    uint8_t status;        // 0x90 note on, 0x80 note off, 0xB0 CC (OR'd with channel - 1)
    uint8_t data1, data2;
    double beat;           // song position of the event, for telemetry and tests
};

template <int Capacity>
struct EventBuffer {
    std::array<MidiEvent, Capacity> events{};
    int size = 0;
    bool overflowed = false;   // set if a block needed more than Capacity events (dropped, never reallocated)
    void clear() { size = 0; overflowed = false; }
    void push(const MidiEvent& e) { if (size < Capacity) events[static_cast<size_t>(size++)] = e; else overflowed = true; }
    /// Stable sort by sample offset (insertion sort: blocks hold few events, and it never allocates).
    void sortByTime() {
        for (int i = 1; i < size; ++i) {
            MidiEvent e = events[static_cast<size_t>(i)]; int j = i - 1;
            while (j >= 0 && events[static_cast<size_t>(j)].sampleOffset > e.sampleOffset) { events[static_cast<size_t>(j + 1)] = events[static_cast<size_t>(j)]; --j; }
            events[static_cast<size_t>(j + 1)] = e;
        }
    }
};

/// What the host tells us about this block (JUCE: AudioPlayHead::PositionInfo).
struct HostBlock {
    double ppqStart = 0;      // song position at the block's first sample, in quarter notes
    double bpm = 120;
    double sampleRate = 48000;
    int numSamples = 512;
    bool playing = true;
};

class Scheduler {
public:
    static constexpr int kMaxPending = 512;
    static constexpr int kMaxVoices = 8;

    template <int Cap>
    void process(const Voice* voices, int voiceCount, const HostBlock& b, EventBuffer<Cap>& out) {
        process(voices, voiceCount, nullptr, 0, b, out);
    }

    /// Step voices and Fibonacci voices together.
    template <int Cap>
    void process(const Voice* voices, int voiceCount, const FibonacciVoice* fibs, int fibCount, const HostBlock& b, EventBuffer<Cap>& out) {
        out.clear();
        if (b.numSamples <= 0 || b.bpm <= 0 || b.sampleRate <= 0) return;
        if (!b.playing) {                       // stopped: release everything once
            if (wasPlaying_) flushAll(out, b.ppqStart);
            wasPlaying_ = false; expected_ = -1; return;
        }
        const double bps = b.bpm / 60.0 / b.sampleRate;          // beats per sample
        // Continuous playback: start exactly where the last block ended, so blocks tile with no floating-point gap or overlap
        // (otherwise an onset sitting on a block edge can be emitted twice or not at all). A real jump is a loop or locate.
        const bool continuous = expected_ >= 0 && std::fabs(b.ppqStart - expected_) <= 1e-6;
        if (expected_ >= 0 && !continuous) flushAll(out, b.ppqStart);  // loop or locate: no hanging notes
        const double start = continuous ? expected_ : b.ppqStart, end = start + b.numSamples * bps;
        wasPlaying_ = true; expected_ = end;

        auto offsetOf = [&](double beat) {
            // +1e-6 sample: positions computed in beats can land a hair under a whole sample (47999.9999999 for 48000),
            // and flooring that would play the note one sample early
            const int o = static_cast<int>(std::floor((beat - start) / bps + 1e-6));
            return o < 0 ? 0 : (o >= b.numSamples ? b.numSamples - 1 : o);
        };

        // 1) note-offs carried over from earlier blocks
        for (int i = 0; i < nPending_;) {
            const Pending& pn = pending_[static_cast<size_t>(i)];   // size_t index: clean under -Wsign-conversion (MPC build)
            if (pn.beat < end) {
                out.push({offsetOf(pn.beat), static_cast<uint8_t>(0x80 | (pn.channel - 1)), pn.note, 0, pn.beat});
                pending_[static_cast<size_t>(i)] = pending_[static_cast<size_t>(--nPending_)];
            } else ++i;
        }

        // 2) new notes: every gate tick whose (micro-shifted, ratcheted) onsets fall in [start, end)
        for (int vi = 0; vi < voiceCount && vi < kMaxVoices; ++vi) {
            const Voice& v = voices[vi];
            const double d = v.lanes[Voice::Gate].stepBeats;
            if (d <= 0) continue;
            int64_t k0 = static_cast<int64_t>(std::floor((start - 0.5 * d) / d)), k1 = static_cast<int64_t>(std::ceil((end + 0.5 * d) / d));
            if (k0 < 0) k0 = 0;
            for (int64_t k = k0; k <= k1; ++k) {
                StepNote n;
                if (!resolveStep(v, k, n)) continue;
                const uint8_t ch = static_cast<uint8_t>(clampi(v.channel, 1, 16) - 1);
                for (int r = 0; r < n.ratchets; ++r) {
                    const double on = n.beat + r * d / n.ratchets;
                    if (on < start || on >= end) continue;
                    if (r == 0 && n.ccA >= 0) out.push({offsetOf(on), static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(v.ccANumber), static_cast<uint8_t>(n.ccA), on});
                    if (r == 0 && n.ccB >= 0) out.push({offsetOf(on), static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(v.ccBNumber), static_cast<uint8_t>(n.ccB), on});
                    out.push({offsetOf(on), static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(n.note), static_cast<uint8_t>(n.velocity), on});
                    const double off = on + n.lengthBeats;
                    if (off < end) out.push({offsetOf(off), static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(n.note), 0, off});
                    else if (nPending_ < kMaxPending) pending_[static_cast<size_t>(nPending_++)] = {off, static_cast<uint8_t>(ch + 1), static_cast<uint8_t>(n.note)};
                    else out.overflowed = true;
                }
            }
        }
        // 3) Fibonacci voices: each arm fires `hits` notes per cycle; hit g sits at g * T / hits (polyrhythm)
        for (int fi = 0; fi < fibCount && fibs; ++fi) {
            const FibonacciVoice& fv = fibs[fi];
            for (const FibArm* arm : {&fv.a, &fv.b}) {
                if (arm->muted || arm->hits <= 0 || fv.cycleBeats <= 0) continue;
                const double dt = fv.cycleBeats / arm->hits;
                int64_t g0 = static_cast<int64_t>(std::floor(start / dt)) - 1, g1 = static_cast<int64_t>(std::ceil(end / dt)) + 1;
                if (g0 < 0) g0 = 0;
                const uint8_t ch = static_cast<uint8_t>(clampi(arm->channel, 1, 16) - 1);
                for (int64_t g = g0; g <= g1; ++g) {
                    const FibNote n = resolveArmHit(fv, *arm, g);
                    if (n.beat < start || n.beat >= end) continue;
                    if (arm->panCC >= 0) out.push({offsetOf(n.beat), static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(arm->panCC), static_cast<uint8_t>(n.pan), n.beat});
                    out.push({offsetOf(n.beat), static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(n.note), static_cast<uint8_t>(n.velocity), n.beat});
                    const double off = n.beat + n.lengthBeats;
                    if (off < end) out.push({offsetOf(off), static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(n.note), 0, off});
                    else if (nPending_ < kMaxPending) pending_[static_cast<size_t>(nPending_++)] = {off, static_cast<uint8_t>(ch + 1), static_cast<uint8_t>(n.note)};
                    else out.overflowed = true;
                }
            }
        }
        out.sortByTime();
    }

    int pendingNoteOffs() const { return nPending_; }
    void reset() { nPending_ = 0; expected_ = -1; wasPlaying_ = false; }

private:
    struct Pending { double beat; uint8_t channel; uint8_t note; };
    template <int Cap> void flushAll(EventBuffer<Cap>& out, double beat) {
        for (int i = 0; i < nPending_; ++i)
            out.push({0, static_cast<uint8_t>(0x80 | (pending_[static_cast<size_t>(i)].channel - 1)), pending_[static_cast<size_t>(i)].note, 0, beat});
        nPending_ = 0;
    }
    std::array<Pending, kMaxPending> pending_{};
    int nPending_ = 0;
    double expected_ = -1;     // where the next block should start if the transport just runs on
    bool wasPlaying_ = false;
};

} // namespace lumina
