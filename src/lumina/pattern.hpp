// Lumina Composer engine: Pattern, a fixed-capacity step pattern.
//
// Header-only, C++17, no dependencies. A Pattern never allocates, so it is safe to build, copy and read on the audio
// thread. Generators that need scratch memory (none here) would run on the message thread and publish a Pattern.
#pragma once
#include <bitset>
#include <cstdint>
#include <string_view>

namespace lumina {

/// The longest lane the engine supports (the spec allows sub-lanes of 1–64 steps).
inline constexpr int kMaxSteps = 64;

/// A gate pattern: `length` steps, each on or off. Index 0 is the first step of the cycle.
struct Pattern {
    std::bitset<kMaxSteps> hits{};
    int length = 0;

    constexpr bool operator[](int i) const { return i >= 0 && i < length && hits[static_cast<size_t>(i)]; }
    void set(int i, bool on = true) { if (i >= 0 && i < length) hits.set(static_cast<size_t>(i), on); }
    int count() const { return static_cast<int>(hits.count()); }

    bool operator==(const Pattern& o) const { return length == o.length && hits == o.hits; }
    bool operator!=(const Pattern& o) const { return !(*this == o); }

    /// Parse "x..x..x." style strings: 'x' or 'X' is a hit, anything else a rest. Longer strings are truncated to kMaxSteps.
    static Pattern parse(std::string_view s) {
        Pattern p;
        p.length = static_cast<int>(s.size() < kMaxSteps ? s.size() : kMaxSteps);
        for (int i = 0; i < p.length; ++i) p.hits.set(static_cast<size_t>(i), s[static_cast<size_t>(i)] == 'x' || s[static_cast<size_t>(i)] == 'X');
        return p;
    }

    /// Rotate so that step `by` becomes step 0 (positive = later steps move earlier, like turning a ring).
    Pattern rotated(int by) const {
        Pattern p; p.length = length;
        if (length == 0) return p;
        int r = ((by % length) + length) % length;
        for (int i = 0; i < length; ++i) p.hits.set(static_cast<size_t>(i), hits[static_cast<size_t>((i + r) % length)]);
        return p;
    }

    /// Swap the two halves (for clave: 3-2 <-> 2-3). Requires an even length; odd lengths are returned unchanged.
    Pattern halvesSwapped() const { return (length % 2) ? *this : rotated(length / 2); }

    /// For tests and the telemetry view: "x..x..x.".
    template <typename Str> void toString(Str& out) const {
        out.clear();
        for (int i = 0; i < length; ++i) out.push_back(hits[static_cast<size_t>(i)] ? 'x' : '.');
    }
};

} // namespace lumina
