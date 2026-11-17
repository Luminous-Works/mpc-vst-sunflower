// Lumina Composer engine: the Fibonacci Sequencer, the author's design.
//
// Source: design notes ("The Fibonacci Sequencer: Vogel Lattice Polyrhythmic Logic",
// vogel_lattice.py), after H. Vogel, "A better way to construct the sunflower head", Mathematical Biosciences 44:179–189 (1979).
//
//   floret n:  theta(n) = n * alpha,  alpha = (3 - sqrt 5) * pi  (the golden angle, ~137.508°),   r(n) = c * sqrt(n)
//   arms:      walk the lattice in Fibonacci strides (34-arm: 1, 35, 69, ...; 55-arm: 1, 56, 111, ...)
//   rhythm:    arm A fires F(k) times in the cycle T while arm B fires F(k+1) times (e.g. 34:55)
//   sound:     radius -> magnitude (velocity, cutoff); angle -> pan / phase (irrational, so it never repeats)
//
// Plus the eight extensions the operator accepted on 16 Nov 2026: arm pairs, divergence angle, Fibonacci word, Pisano melodies,
// Zeckendorf accents, golden rotation, Fibonacci phrase form, and the 13-floret keybed.
// Everything here is pure, deterministic and allocation-free.
#pragma once
#include "pattern.hpp"
#include <cmath>
#include <cstdint>

namespace lumina::fib {

inline constexpr double kPhi = 1.6180339887498948482;
inline constexpr double kPi = 3.14159265358979323846;
/// The golden angle in radians: (3 - sqrt 5) * pi = 2*pi / phi^2 ~ 137.50776°.
inline constexpr double kGoldenAngle = (3.0 - 2.2360679774997896964) * kPi;
inline double degrees(double rad) { return rad * 180.0 / kPi; }
inline double radians(double deg) { return deg * kPi / 180.0; }

/// F(0)=0, F(1)=1, ... ; exact up to F(92).
inline constexpr uint64_t F(int n) {
    uint64_t a = 0, b = 1;
    for (int i = 0; i < n; ++i) { const uint64_t t = a + b; a = b; b = t; }
    return a;
}

// ---------------------------------------------------------------------------------------------------------------------
// Vogel lattice and arms (core)

struct Floret { double r, theta, x, y; };

/// Floret n of the lattice. `divergence` is the angle between successive florets: kGoldenAngle for the perfect sunflower,
/// and the operator's Divergence control moves it off (extension 2).
inline Floret floret(int n, double c = 1.0, double divergence = kGoldenAngle) {
    const double r = c * std::sqrt(static_cast<double>(n));
    const double th = n * divergence;
    return {r, th, r * std::cos(th), r * std::sin(th)};
}

/// The arm pairs (extension 1): consecutive Fibonacci strides, from 3:5 to 55:89.
struct ArmPair { int a, b; };
inline constexpr ArmPair kArmPairs[] = {{3, 5}, {5, 8}, {8, 13}, {13, 21}, {21, 34}, {34, 55}, {55, 89}};

/// The m-th floret index on the arm that starts at `start` and steps by `stride` (start + m * stride).
inline constexpr int armIndex(int start, int stride, int m) { return start + m * stride; }

/// Onset time of hit i of `count` hits spread evenly over a cycle of length T (Δt = T / count).
/// For 34:55, arm A uses count 34 and arm B count 55 over the same T, so they realign only at the cycle start.
inline constexpr double onsetTime(int i, int count, double T) { return count > 0 ? T * i / count : 0.0; }

/// Radius-driven magnitude in [0,1] for the m-th floret of an arm, normalised to the lattice size N:
/// sqrt(n / N). The faster arm (bigger stride) grows in magnitude sooner, as notes describe.
inline double armMagnitude(int start, int stride, int m, int N) {
    const int n = armIndex(start, stride, m);
    if (N <= 0) return 0.0;
    const double v = std::sqrt(static_cast<double>(n) / N);
    return v > 1.0 ? 1.0 : v;
}

/// Angle-driven pan in [-1, 1] for floret n: cos(theta(n)). With the golden angle it never cycles.
inline double armPan(int n, double divergence = kGoldenAngle) { return std::cos(n * divergence); }

// ---------------------------------------------------------------------------------------------------------------------
// Extension 3: the Fibonacci word (Sturmian). S0 = "b", S1 = "a", Sn = S(n-1) S(n-2)  ->  abaababaabaab...
// Closed (mechanical-word) form, so any step is computed directly with no string building:
//   step i (0-based) is 'a' (a hit)  <=>  floor((i + 2) / phi) - floor((i + 1) / phi) == 1.
// tests/test_main.cpp checks this against the recursive construction for the first 64 steps.

/// A Fibonacci-word gate pattern of `length` steps: hit ('a') or rest ('b'). Hit density tends to 1/phi.
inline Pattern word(int length) {
    Pattern p; p.length = length > kMaxSteps ? kMaxSteps : (length < 0 ? 0 : length);
    for (int i = 0; i < p.length; ++i) {
        const long long a = static_cast<long long>(std::floor((i + 2) / kPhi));
        const long long b = static_cast<long long>(std::floor((i + 1) / kPhi));
        p.hits.set(static_cast<size_t>(i), (a - b) == 1);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extension 4: Pisano melodies. F(n) mod m repeats with period pi(m): pi(7)=16 (one bar of 16ths over a diatonic scale),
// pi(12)=24, pi(5)=20, pi(13)=28 (13-key Fibonacci octave).

inline int pisanoPeriod(int m) {
    if (m <= 1) return 1;
    int a = 0, b = 1;
    for (int i = 1; i <= 6 * m + 6; ++i) {  // pi(m) <= 6m
        const int t = (a + b) % m; a = b; b = t;
        if (a == 0 && b == 1) return i;
    }
    return 0;
}

/// F(i) mod m, the i-th scale degree of a Pisano melody over an m-note scale.
inline int pisanoDegree(int i, int m) {
    if (m <= 1) return 0;
    int a = 0, b = 1;
    const int p = pisanoPeriod(m);
    const int n = p ? i % p : i;
    for (int k = 0; k < n; ++k) { const int t = (a + b) % m; a = b; b = t; }
    return a;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extension 5: Zeckendorf accents. Every n >= 1 is a unique sum of non-consecutive Fibonacci numbers (F(2) = 1 upward).

struct Zeckendorf { int terms; uint64_t largest; uint64_t mask; };  // mask bit j set = F(j + 2) used

inline Zeckendorf zeckendorf(uint64_t n) {
    Zeckendorf z{0, 0, 0};
    int j = 2; while (F(j + 1) <= n) ++j;            // largest F(j) <= n
    for (; j >= 2 && n > 0; --j) {
        if (F(j) <= n) { n -= F(j); ++z.terms; z.mask |= (1ull << (j - 2)); if (!z.largest) z.largest = F(j); --j; }
    }
    return z;
}

/// Accent level for step i (1-based count): more Zeckendorf terms = weaker. Returns 1.0 for Fibonacci numbers themselves.
inline double zeckendorfAccent(int step) {
    if (step <= 0) return 1.0;
    return 1.0 / zeckendorf(static_cast<uint64_t>(step)).terms;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extension 6: golden rotation. Lane rotation for bar b = floor(b * phi) mod n: drifts and does not return in a short span.
inline int goldenRotation(int bar, int n) {
    if (n <= 0) return 0;
    const long long r = static_cast<long long>(std::floor(bar * kPhi));
    return static_cast<int>(((r % n) + n) % n);
}

// ---------------------------------------------------------------------------------------------------------------------
// Extension 7: Fibonacci phrase form. Section lengths in bars: 3, 5, 8, 13.
inline constexpr int kPhraseBars[] = {3, 5, 8, 13};

// ---------------------------------------------------------------------------------------------------------------------
// Extension 8: the 13-floret keybed (MODEL ϕ-233): the first 13 florets are the 13 tuning nodes C .. C8ve.
// Semitone offset from the root for keybed node n (0..12) is simply n; the floret gives its position and colour.
inline constexpr int kKeybedNodes = 13;

} // namespace lumina::fib
