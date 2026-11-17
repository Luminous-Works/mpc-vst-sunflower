// Lumina Composer engine: Euclidean rhythms, the clave library and the Deviation measure.
//
// Euclidean is just a mode in Lumina Composer (the operator, 16 Nov 2026). The flagship is the Fibonacci Sequencer: see fibonacci.hpp.
#pragma once
#include "pattern.hpp"
#include <cmath>
#include <string_view>

namespace lumina {

/// E(k, n, r): k hits spread as evenly as possible over n steps, rotated by r.
/// Step i is a hit when ((i + r) * k) mod n < k. This closed form produces the same necklaces as Bjorklund's algorithm
/// (Toussaint 2005); the rotation that puts a hit on step 0 differs between published tables, so tests compare
/// patterns up to rotation where the literature does.
inline Pattern euclid(int k, int n, int r = 0) {
    Pattern p;
    if (n <= 0) return p;
    p.length = n > kMaxSteps ? kMaxSteps : n;
    if (k <= 0) return p;
    if (k >= p.length) { for (int i = 0; i < p.length; ++i) p.hits.set(static_cast<size_t>(i)); return p; }
    const int rr = ((r % p.length) + p.length) % p.length;
    for (int i = 0; i < p.length; ++i)
        p.hits.set(static_cast<size_t>(i), (static_cast<long long>(i + rr) * k) % p.length < k);
    return p;
}

/// True if a and b are the same pattern up to rotation (same necklace).
inline bool sameNecklace(const Pattern& a, const Pattern& b) {
    if (a.length != b.length || a.count() != b.count()) return false;
    for (int r = 0; r < a.length; ++r) if (a.rotated(r) == b) return true;
    return a.length == 0;
}

/// Inter-onset intervals (gaps between hits, wrapping round the cycle), written into `out`; returns how many.
inline int interOnsetIntervals(const Pattern& p, int (&out)[kMaxSteps]) {
    int first = -1, prev = -1, n = 0;
    for (int i = 0; i < p.length; ++i) {
        if (!p[i]) continue;
        if (first < 0) first = i; else out[n++] = i - prev;
        prev = i;
    }
    if (first >= 0) out[n++] = p.length - prev + first;  // wrap to the first hit of the next cycle
    return n;
}

/// Deviation from maximal evenness: the RMS distance of the inter-onset intervals from the ideal n/k, divided by n/k.
/// 0 for any Euclidean pattern whose gaps all equal n/k; clave sits clearly above the Euclidean pattern with the same k and n.
inline double deviation(const Pattern& p) {
    int ioi[kMaxSteps]; const int k = interOnsetIntervals(p, ioi);
    if (k == 0) return 0.0;
    const double ideal = static_cast<double>(p.length) / k;
    double s = 0; for (int i = 0; i < k; ++i) { const double d = ioi[i] - ideal; s += d * d; }
    return std::sqrt(s / k) / ideal;
}

/// The clave and bell library, 16 steps (two bars of 8th notes; one 4/4 bar of 16ths reads the same way), written
/// "3-2" where it applies. 2-3 is Pattern::halvesSwapped(). Patterns from research/03-instrument-profiles.md.
namespace clave {
inline Pattern son32()     { return Pattern::parse("x..x..x...x.x..."); }  // gaps 3-3-4-2-4
inline Pattern son23()     { return son32().halvesSwapped(); }
inline Pattern rumba32()   { return Pattern::parse("x..x...x..x.x..."); }  // gaps 3-4-3-2-4
inline Pattern rumba23()   { return rumba32().halvesSwapped(); }
inline Pattern bossa()     { return Pattern::parse("x..x..x...x..x.."); }  // gaps 3-3-4-3-3
inline Pattern tresillo()  { return Pattern::parse("x..x..x."); }          // E(3,8)
inline Pattern cinquillo() { return Pattern::parse("x.xx.xx."); }          // E(5,8)
} // namespace clave

} // namespace lumina
