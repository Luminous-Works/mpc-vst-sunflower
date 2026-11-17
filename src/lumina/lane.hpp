// Lumina Composer engine: lanes.
//
// A voice is built from sub-lanes (gate, pitch, velocity, probability, ratchet, micro-timing, CC A, CC B). Every lane has
// its OWN length (1–64), direction and clock division. That independence is the polymeter in the spec: a 13-step pitch lane
// over a 7-step gate and an 11-step velocity lane only realigns after lcm(13, 7, 11) = 1001 steps.
// Lanes are plain values with fixed-size storage: copying or reading one never allocates.
#pragma once
#include "pattern.hpp"
#include <array>
#include <cstdint>

namespace lumina {

enum class Direction : uint8_t { Forward, Reverse, PingPong, Random };

/// Deterministic 64-bit mix (SplitMix64). Same inputs -> same output on every machine, so "random" lanes and probability
/// rolls are reproducible from a seed (the spec's Lock Seed).
inline constexpr uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
/// Uniform double in [0, 1) from (seed, a, b).
inline constexpr double unit(uint64_t seed, uint64_t a, uint64_t b = 0) {
    return static_cast<double>(mix64(seed ^ mix64(a * 0x100000001B3ull + b)) >> 11) * (1.0 / 9007199254740992.0);
}

/// Which of a lane's `length` cells plays on the lane's k-th clock tick (k counts from the start of the song, k >= 0).
inline int laneIndex(int64_t k, int length, Direction dir, uint64_t seed = 0) {
    if (length <= 1) return 0;
    if (k < 0) k = 0;
    switch (dir) {
        case Direction::Forward:  return static_cast<int>(k % length);
        case Direction::Reverse:  return length - 1 - static_cast<int>(k % length);
        case Direction::PingPong: {                       // 0 1 2 3 2 1 0 1 2 ...  (period 2L - 2, ends not repeated)
            const int64_t period = 2 * static_cast<int64_t>(length) - 2;
            const int p = static_cast<int>(k % period);
            return p < length ? p : static_cast<int>(period) - p;
        }
        case Direction::Random:   return static_cast<int>(mix64(seed ^ static_cast<uint64_t>(k)) % static_cast<uint64_t>(length));
    }
    return 0;
}

/// One lane. `stepBeats` is the clock: the length of one cell in quarter-note beats (1/16 note = 0.25, 1/8 = 0.5,
/// 1/8 triplet = 1/3). Gate lanes use `gates`; value lanes use `values` (meaning depends on the lane, see Voice).
struct Lane {
    int length = 16;
    Direction direction = Direction::Forward;
    double stepBeats = 0.25;
    Pattern gates{};                    // gate lane: which cells fire
    std::array<float, kMaxSteps> values{};  // value lanes: one value per cell

    /// The clock tick (k) in force at song position `beat` (quarter notes from song start).
    int64_t tickAt(double beat) const { return stepBeats > 0 ? static_cast<int64_t>(beat / stepBeats + 1e-9) : 0; }
    int cellAt(double beat, uint64_t seed = 0) const { return laneIndex(tickAt(beat), length, direction, seed); }
    float valueAt(double beat, uint64_t seed = 0) const { return values[static_cast<size_t>(cellAt(beat, seed))]; }

    static Lane gate(const Pattern& p, double stepBeats = 0.25) { Lane l; l.length = p.length; l.gates = p; l.stepBeats = stepBeats; return l; }
    static Lane constant(float v, int length = 1, double stepBeats = 0.25) {
        Lane l; l.length = length; l.stepBeats = stepBeats; l.values.fill(v); return l;
    }
};

} // namespace lumina
