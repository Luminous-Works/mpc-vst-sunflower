// Lumina Composer engine: a voice combines its sub-lanes into notes.
//
// The GATE lane decides when a step may sound (its own length, direction and clock). At each gate onset the other lanes are
// read at that same song position, each through its OWN clock, so they drift against the gate (polymeter):
//   pitch       semitones added to `rootNote`          (value lane)
//   velocity    0..1 -> MIDI 1..127                     (value lane)
//   probability 0..1 chance the step plays; the roll is seeded, so a locked seed always plays the same notes
//   ratchet     1..8 repeats squeezed into the step    (value, rounded)
//   micro       -0.5..+0.5 of a step: timing push/pull (value lane)
//   ccA, ccB    0..1 -> CC value 0..127, sent with each note on the voice's CC numbers
#pragma once
#include "lane.hpp"
#include <cmath>

namespace lumina {

struct Voice {
    enum LaneId { Gate, Pitch, Velocity, Probability, Ratchet, Micro, CcA, CcB, LaneCount };

    std::array<Lane, LaneCount> lanes{};
    int channel = 1;          // MIDI channel 1..16 (or the hosted instrument's input)
    int rootNote = 60;
    float gateLength = 0.5f;  // fraction of the (ratcheted) step the note is held
    int ccANumber = 74, ccBNumber = 71;
    uint64_t seed = 0x1658;   // per-voice seed (Lock Seed keeps it)
    bool muted = false;

    Voice() {
        lanes[Gate] = Lane::gate(Pattern::parse("x"), 0.25);
        lanes[Pitch] = Lane::constant(0.0f);
        lanes[Velocity] = Lane::constant(0.8f);
        lanes[Probability] = Lane::constant(1.0f);
        lanes[Ratchet] = Lane::constant(1.0f);
        lanes[Micro] = Lane::constant(0.0f);
        lanes[CcA] = Lane::constant(-1.0f);   // negative = lane off (no CC sent)
        lanes[CcB] = Lane::constant(-1.0f);
    }
};

/// One resolved note from a gate onset, before it is cut into ratchets and placed in a block.
struct StepNote {
    double beat;       // onset in song beats (micro-timing applied)
    double lengthBeats;
    int note, velocity, ratchets;
    int ccA, ccB;      // -1 = none
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Resolve gate tick k of voice v into a note. Returns false if the step is silent (no gate, failed probability, muted).
inline bool resolveStep(const Voice& v, int64_t k, StepNote& out) {
    if (v.muted) return false;
    const Lane& g = v.lanes[Voice::Gate];
    const int cell = laneIndex(k, g.length, g.direction, v.seed);
    if (!g.gates[cell]) return false;

    const double t = static_cast<double>(k) * g.stepBeats;   // nominal onset, in beats
    const double prob = v.lanes[Voice::Probability].valueAt(t, v.seed);
    if (prob < 1.0 && unit(v.seed, static_cast<uint64_t>(k), 0x9B) >= prob) return false;

    const double micro = v.lanes[Voice::Micro].valueAt(t, v.seed);
    out.beat = t + (micro < -0.5 ? -0.5 : (micro > 0.5 ? 0.5 : micro)) * g.stepBeats;
    out.ratchets = clampi(static_cast<int>(std::lround(v.lanes[Voice::Ratchet].valueAt(t, v.seed))), 1, 8);
    out.lengthBeats = g.stepBeats / out.ratchets * v.gateLength;
    out.note = clampi(v.rootNote + static_cast<int>(std::lround(v.lanes[Voice::Pitch].valueAt(t, v.seed))), 0, 127);
    out.velocity = clampi(static_cast<int>(std::lround(v.lanes[Voice::Velocity].valueAt(t, v.seed) * 126.0)) + 1, 1, 127);
    const float a = v.lanes[Voice::CcA].valueAt(t, v.seed), b = v.lanes[Voice::CcB].valueAt(t, v.seed);
    out.ccA = a < 0 ? -1 : clampi(static_cast<int>(std::lround(a * 127.0)), 0, 127);
    out.ccB = b < 0 ? -1 : clampi(static_cast<int>(std::lround(b * 127.0)), 0, 127);
    return true;
}

} // namespace lumina
