// Lumina Composer engine: the Fibonacci voice. Vogel-lattice design, made playable.
//
// A FibonacciVoice has two arms. Over one cycle of `cycleBeats`, arm A fires `hits` notes evenly spaced (Δt = T / hits)
// and arm B fires its own count over the same T: with the default arm pair 34:55 that is 34-against-55
// polyrhythm, and the two arms meet only on the cycle's downbeat.
//
// Each hit walks one floret along the arm: floret n = start + m * stride (34-arm: 1, 35, 69, ...). From that floret:
//   magnitude  sqrt(n / N)   -> velocity (velMin..velMax): the arm swells as it travels out across the sunflower head
//   angle      n * divergence -> pan (CC 10): golden angle = never the same place twice
//   pitch      by PitchMode:
//                Fixed       every hit on the root
//                Keybed13    the floret's angle picks one of the 13 keybed nodes (Fibonacci Keybed, MODEL ϕ-233)
//                Pisano      F(hit index) mod scale size, read through a scale (Pisano melody; mod 7 repeats every 16)
// When the walk passes the edge of the lattice (n >= N) it continues on the next arm of the same family (start + 1), so a
// long performance travels every spiral of the family, as the mockup's sunflower view shows.
#pragma once
#include "fibonacci.hpp"
#include "voice.hpp"
#include <array>
#include <cmath>

namespace lumina {

enum class FibPitch : uint8_t { Fixed, Keybed13, Pisano };

struct FibArm {
    int stride = 34;              // a Fibonacci number: the spiral family
    int start = 1;                // first floret (notes start both arms at 1)
    int hits = 34;                // notes per cycle; by default equal to the stride (34:55)
    int channel = 1;
    int rootNote = 60;
    FibPitch pitch = FibPitch::Keybed13;
    std::array<int, 12> scale{0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19};  // A natural minor shape, in semitones (Pisano mode)
    int scaleSize = 7;            // Pisano modulus: 7 = diatonic (period 16), 5 = pentatonic (20), 12 = chromatic (24)
    int velMin = 40, velMax = 120;
    float gate = 0.5f;            // fraction of the hit spacing the note is held
    int panCC = 10;               // -1 = don't send pan
    bool muted = false;
};

struct FibonacciVoice {
    double cycleBeats = 16.0;     // T: 4 bars of 4/4
    int latticeSize = 610;        // N: a Fibonacci number of florets
    double divergence = fib::kGoldenAngle;  // the Divergence control (radians)
    FibArm a{}, b{};

    FibonacciVoice() { b.stride = 55; b.hits = 55; b.channel = 2; b.rootNote = 72; }

    /// Arm pair from the panel's chips (3:5 ... 55:89): strides and hit counts.
    void setArmPair(int strideA, int strideB) { a.stride = a.hits = strideA; b.stride = b.hits = strideB; }
};

/// How many florets lie on one arm (start, start + stride, ... < N).
inline int armLength(int start, int stride, int N) { return (stride <= 0 || start >= N) ? 1 : (N - 1 - start) / stride + 1; }

/// The floret for global hit g of an arm (walks the arm, then the next arm of the family, and so on).
inline int armFloret(const FibArm& arm, int64_t g, int N) {
    if (arm.stride <= 0) return arm.start;
    int start = arm.start;
    for (int guard = 0; guard < arm.stride; ++guard) {          // walk at most one full family before wrapping
        const int len = armLength(start, arm.stride, N);
        if (g < len) return start + static_cast<int>(g) * arm.stride;
        g -= len; start = arm.start + ((start - arm.start + 1) % arm.stride);
    }
    return arm.start + static_cast<int>(g % armLength(arm.start, arm.stride, N)) * arm.stride;
}

/// Resolve global hit g of an arm into a note (onset, length, pitch, velocity, pan).
struct FibNote { double beat, lengthBeats; int note, velocity, pan; int floret; };

inline FibNote resolveArmHit(const FibonacciVoice& v, const FibArm& arm, int64_t g) {
    const int hits = arm.hits > 0 ? arm.hits : 1;
    const double dt = v.cycleBeats / hits;
    const int64_t cycle = g / hits, i = g % hits;
    // period of the floret walk: one pass over the whole family
    int64_t family = 0; for (int s = 0; s < arm.stride && s < v.latticeSize; ++s) family += armLength(arm.start + s >= v.latticeSize ? 0 : arm.start + s, arm.stride, v.latticeSize);
    const int n = armFloret(arm, family > 0 ? g % family : g, v.latticeSize);

    FibNote out{};
    out.beat = static_cast<double>(cycle) * v.cycleBeats + static_cast<double>(i) * dt;
    out.lengthBeats = dt * arm.gate;
    out.floret = n;
    const double mag = std::sqrt(static_cast<double>(n) / v.latticeSize);
    out.velocity = clampi(static_cast<int>(std::lround(arm.velMin + (arm.velMax - arm.velMin) * (mag > 1 ? 1 : mag))), 1, 127);
    const double ang = std::fmod(n * v.divergence, 2 * fib::kPi);
    out.pan = clampi(static_cast<int>(std::lround((std::cos(ang) * 0.5 + 0.5) * 127.0)), 0, 127);
    int semis = 0;
    switch (arm.pitch) {
        case FibPitch::Fixed: semis = 0; break;
        case FibPitch::Keybed13: semis = static_cast<int>(ang / (2 * fib::kPi) * fib::kKeybedNodes) % fib::kKeybedNodes; break;  // node 0..12
        case FibPitch::Pisano: {
            const int m = arm.scaleSize < 1 ? 1 : (arm.scaleSize > 12 ? 12 : arm.scaleSize);
            semis = arm.scale[static_cast<size_t>(fib::pisanoDegree(static_cast<int>(g % 1000000), m))];
        } break;
    }
    out.note = clampi(arm.rootNote + semis, 0, 127);
    return out;
}

} // namespace lumina
