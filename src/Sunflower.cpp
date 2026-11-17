// Sunflower Fibonacci Sequencer - an MPC instrument by Lumina Aerospace LLC.   SPDX-License-Identifier: MIT
//
// THE IDEA
//   A sunflower packs its seeds on two families of spirals, and the spiral counts are neighbouring Fibonacci numbers
//   (34 and 55 on the big ones). This instrument plays that lattice: two "arms" step through the florets, one note per
//   floret, and the pair you choose (3:5 ... 34:55) sets how fast each arm turns against the other. Because the two
//   cycles are coprime they only meet once per cycle, so the melody and the bass line keep drifting and re-locking
//   (polymeter) the way a Fibonacci spiral does. A small percussion bed (kick, clave/rim, conga, shaker) sits underneath.
//
// ENGINE
//   The note logic is the lumina engine (src/lumina/*.hpp, header-only C++17, the same code as the Lumina Composer
//   plugin and its 113,757 tests). Each audio block the host's song position (ppq) goes into lumina::Scheduler, which
//   returns note events with sample offsets; they trigger the voices below. Block-size invariant: the same notes fall at
//   the same song positions whatever the block size. The engine is allocation-free; so is process().
//
// SOUND
//   Plucks (arm A melody, arm B bass): sine + octave partial + a touch of third, exponential decay.
//   Percussion: synthesised (sine kick with pitch drop, clave, conga, filtered-noise shaker). It is a self-sounding
//   instrument on purpose: the MPC host ignores MIDI sent out by plugins, so a sequencer has to make its own sound.
//
// LIVE PLAYHEAD (display only)
//   The last parameter, "Playhead", is not a control: it holds which of 64 pictures of the sunflower the touch screen should show, i.e. where the
//   transport is inside the arms' cycle (a bright comet sweeping the lattice once per cycle). process() writes the frame into an atomic; a small
//   thread looks at it about every 100 ms and, only when it changed, sets the parameter and tells the host (audioMasterAutomate). That is the
//   mechanism other MPC plugins use for live meters. It is NOT automatable (so it never floods project automation), and we never ask the host to
//   rebuild its display (that closes popups). About 10 updates a second: a beat-level playhead, not a smooth sweep.
//
// Hard rules honoured: parameter 0 is Output (harmless when the host nudges it); no allocation, locks or I/O in
// process(); uid "L5cL" matches plugin.json; parameter indices are append-only.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"

#include "lumina/scheduler.hpp"
#include "lumina/fib_voice.hpp"
#include "lumina/voice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0,    // dB, master (harmless on host nudge)
    kArms,          // choice: the Fibonacci pair 3:5 .. 34:55
    kPitch,         // choice: Keybed 13 / Pisano / Fixed
    kRoot,          // choice: key C..B
    kMode,          // choice: Minor / Major (the Pisano scale shape)
    kCycle,         // choice: cycle length 2 / 4 / 8 bars
    kDiverge,       // degrees, the golden angle by default
    kGroove,        // choice: Salsa / Reggae / Cumbia / Bossa Nova / Off
    kArmLevel,      // %
    kPercLevel,     // %
    kDecay,         // % pluck length
    kPlayhead,      // display only, NOT automatable: the touch screen's playhead picture (0..63), written by the plugin itself
    kNumParams
};
constexpr int kPlayFrames = 64;

constexpr int kArmStrides[6][2] = {{3, 5}, {5, 8}, {8, 13}, {13, 21}, {21, 34}, {34, 55}};
constexpr int kCycleBars[3] = {2, 4, 8};
// Modes: the first two keep the original Minor / Major indices (parameter order is append-only), the rest follow.
// Names are short (<= 12 chars) to fit the MPC's value field. `n` notes per octave, semitones from the key.
struct Mode { const char* name; int n; int semi[7]; };
constexpr int kNumModes = 14;
constexpr Mode kModes[kNumModes] = {
    {"Minor",       7, {0, 2, 3, 5, 7, 8, 10}},   // Aeolian
    {"Major",       7, {0, 2, 4, 5, 7, 9, 11}},   // Ionian
    {"Dorian",      7, {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",    7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",      7, {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",  7, {0, 2, 4, 5, 7, 9, 10}},
    {"Locrian",     7, {0, 1, 3, 5, 6, 8, 10}},
    {"Harm Minor",  7, {0, 2, 3, 5, 7, 8, 11}},
    {"Mel Minor",   7, {0, 2, 3, 5, 7, 9, 11}},
    {"Pent Major",  5, {0, 2, 4, 7, 9}},
    {"Pent Minor",  5, {0, 3, 5, 7, 10}},
    {"Blues",       6, {0, 3, 5, 6, 7, 10}},
    {"Phryg Dom",   7, {0, 1, 4, 5, 7, 8, 10}},  // Phrygian dominant (Spanish)
    {"Whole Tone",  6, {0, 2, 4, 6, 8, 10}},
};

// Percussion on MIDI channels 3..6 of the engine's voices: kick, clave/rim, conga, shaker. 32 sixteenths = 2 bars.
struct Groove { const char* name; const char* pat[4]; };
constexpr Groove kGrooves[5] = {
    {"Salsa",      {"........x.......x.......x.......", "x..x..x.........|....x...x.......", "..x.x.xx..x.x.xx..x.x.xx..x.x.xx", "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x."}},
    {"Reggae",     {"........x.......|........x.......", "........x.......|........x.......", "..x...x...x...x.|..x...x...x...x.", "x.x.x.x.x.x.x.x.|x.x.x.x.x.x.x.x."}},
    {"Cumbia",     {"x.......x.......|x.......x.......", "..x...x...x...x.|..x...x...x...x.", "..x...x...x...x.|..x...x...x...x.", "x.xxx.xxx.xxx.xx|x.xxx.xxx.xxx.xx"}},
    {"Bossa Nova", {"x.......x.......|x.......x.......", "x..x..x...x..x..|x..x..x...x..x..", "........x.......|........x.......", "x.x.x.x.x.x.x.x.|x.x.x.x.x.x.x.x."}},
    {"Off",        {"", "", "", ""}},
};

lumina::Pattern parsePat(const char* s)
{
    char buf[40]; int n = 0;
    for (; *s && n < 32; ++s) if (*s != '|') buf[n++] = *s;
    buf[n] = 0;
    return lumina::Pattern::parse(n ? buf : ".");
}

// one voice: a decaying partial stack or a noise burst; pans with constant power
struct Hit
{
    bool active = false;
    int kind = 0;                 // 0 pluck, 1 kick, 2 clave, 3 conga, 4 shaker
    float phase = 0, inc = 0, amp = 0, coef = 0, gain = 0, pitchEnv = 0, pitchCoef = 0, baseInc = 0, l = 0, r = 0;
    float lp = 0;                 // shaker one-pole state
};

} // namespace

class Sunflower : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Sunflower";
        i.vendor = "Lumina Aerospace LLC";
        i.uniqueId = MPC_FOURCC('L', '5', 'c', 'L');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Sunflower() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out", "Output", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("arms", "Arms", {"3:5", "5:8", "8:13", "13:21", "21:34", "34:55"}, 5));
        addParam(choiceParam("pitch", "Pitch", {"Keybed", "Pisano", "Fixed"}, 0));
        addParam(choiceParam("root", "Key", {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"}, 9));
        {
            std::vector<std::string> names;
            for (const auto& m : kModes) names.push_back(m.name);
            addParam(choiceParam("mode", "Mode", names, 0));
        }
        addParam(choiceParam("cycle", "Cycle", {"2 bars", "4 bars", "8 bars"}, 1));
        addParam(floatParam("diverge", "Divergence", 130.0f, 145.0f, 137.508f, "deg", 3));
        addParam(choiceParam("groove", "Groove", {"Salsa", "Reggae", "Cumbia", "Bossa Nova", "Off"}, 0));
        addParam(floatParam("armlvl", "Arm Level", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(floatParam("perclvl", "Perc Level", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam("decay", "Pluck Decay", 0.0f, 100.0f, 50.0f, "%", 0));
        {
            Param ph = floatParam("playhead", "Playhead", 0.0f, (float)(kPlayFrames - 1), 0.0f, "", 0);
            ph.automatable = false;   // display only: never recorded into project automation
            addParam(ph);
        }
        if (numParams() != kNumParams) mpc::log("Sunflower: parameter count mismatch");
        run_.store(true);
        poll_ = std::thread([this] { pollLoop(); });

        std::vector<float> v((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
        addProgram("Sunflower 34:55", v);
        v[kArms] = 4; v[kGroove] = 1; v[kRoot] = 7; v[kPitch] = 0; addProgram("One Drop Spiral", v);               // 21:34, Reggae, G
        v[kArms] = 2; v[kGroove] = 3; v[kRoot] = 0; v[kMode] = 1; v[kPitch] = 1; addProgram("Bossa Seeds", v);     // 8:13, Bossa, C major, Pisano
        v[kArms] = 1; v[kGroove] = 2; v[kRoot] = 2; v[kMode] = 0; v[kPitch] = 0; addProgram("Cumbia Florets", v);
        v[kArms] = 5; v[kGroove] = 4; v[kRoot] = 9; v[kCycle] = 2; v[kPitch] = 0; addProgram("Arms Only", v);
    }

    ~Sunflower() override
    {
        run_.store(false);
        if (poll_.joinable()) poll_.join();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        rebuilt_ = false;
        reset();
    }

    void reset() override
    {
        for (auto& h : hits_) h.active = false;
        sched_.reset();
        free_ppq_ = 0.0;
        panA_ = panB_ = 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        rebuildIfChanged();

        // host clock -> engine block. No clock from the host: free-run at 120 bpm so the plugin is never silent in a simulator.
        lumina::HostBlock hb;
        hb.numSamples = ctx.numFrames; hb.sampleRate = ctx.sampleRate;
        if (ctx.transport.valid)
        {
            hb.bpm = ctx.transport.tempoValid ? ctx.transport.bpm : 120.0;
            hb.ppqStart = ctx.transport.ppqValid ? ctx.transport.ppqPos : free_ppq_;
            hb.playing = ctx.transport.playing;
        }
        else { hb.bpm = 120.0; hb.ppqStart = free_ppq_; hb.playing = true; }
        free_ppq_ = hb.ppqStart + hb.bpm / 60.0 * ctx.numFrames / ctx.sampleRate;

        // where the transport is inside the arms' cycle, as a picture number for the touch screen (read by pollLoop)
        {
            int fr = 0;
            if (hb.playing && fib_.cycleBeats > 0.0)
            {
                double ph = std::fmod(hb.ppqStart, fib_.cycleBeats) / fib_.cycleBeats;
                if (ph < 0.0) ph += 1.0;
                fr = std::min(kPlayFrames - 1, static_cast<int>(ph * kPlayFrames));
            }
            frame_.store(fr, std::memory_order_relaxed);
        }

        sched_.process(voices_.data(), kPercVoices, &fib_, 1, hb, ev_);
        ev_.sortByTime();

        // host all-notes-off / all-sound-off: silence the voices (the sequencer itself follows the transport)
        for (int m = 0; m < ctx.numMidiEvents; ++m)
            if (ctx.midi[m].isAllNotesOff()) for (auto& h : hits_) h.active = false;

        const float gOut = dbToGain(get(kOutput));
        int pos = 0;
        for (int e = 0; e < ev_.size; ++e)
        {
            const lumina::MidiEvent& me = ev_.events[(size_t)e];
            const int off = std::min(std::max(me.sampleOffset, 0), ctx.numFrames);
            if (off > pos) { render(ctx, pos, off, gOut); pos = off; }
            handle(me);
        }
        render(ctx, pos, ctx.numFrames, gOut);
    }

private:
    static constexpr int kPercVoices = 4;      // engine voices (channels 3..6)
    static constexpr int kPool = 24;

    // engine setup from the parameters (only when one changed)
    void rebuildIfChanged()
    {
        float cur[kPlayhead];   // every parameter EXCEPT the display-only playhead (it moves on its own, constantly)
        for (int i = 0; i < kPlayhead; ++i) cur[i] = get(i);
        if (rebuilt_ && std::memcmp(cur, last_, sizeof cur) == 0) return;
        std::memcpy(last_, cur, sizeof cur); rebuilt_ = true;

        const int ai = std::min(std::max(getInt(kArms), 0), 5);
        fib_ = lumina::FibonacciVoice{};
        fib_.setArmPair(kArmStrides[ai][0], kArmStrides[ai][1]);
        fib_.cycleBeats = 4.0 * kCycleBars[std::min(std::max(getInt(kCycle), 0), 2)];
        fib_.divergence = (double)get(kDiverge) * 3.14159265358979323846 / 180.0;
        const int root = std::min(std::max(getInt(kRoot), 0), 11);
        fib_.a.channel = 1; fib_.a.rootNote = 60 + root;
        fib_.b.channel = 2; fib_.b.rootNote = 28 + ((root + 8) % 12);   // tonic between E1 and D#2 (28..39): a bass register
        fib_.a.velMin = 45; fib_.a.velMax = 115; fib_.b.velMin = 70; fib_.b.velMax = 118;
        const int pm = getInt(kPitch);
        const lumina::FibPitch fp = pm == 1 ? lumina::FibPitch::Pisano : (pm == 2 ? lumina::FibPitch::Fixed : lumina::FibPitch::Keybed13);
        fib_.a.pitch = fib_.b.pitch = fp;
        // the mode: Pisano indexes the 12-entry scale (n notes, repeated up the octaves, modulus = n); every pitch mode is then
        // snapped to the mode in handle(), so Keybed 13 and Fixed follow it too
        const Mode& md = kModes[std::min(std::max(getInt(kMode), 0), kNumModes - 1)];
        for (int i = 0; i < 12; ++i) fib_.a.scale[(size_t)i] = fib_.b.scale[(size_t)i] = md.semi[i % md.n] + 12 * (i / md.n);
        fib_.a.scaleSize = fib_.b.scaleSize = md.n;
        modeMask_ = 0; for (int i = 0; i < md.n; ++i) modeMask_ |= (unsigned)(1u << md.semi[i]);
        root_ = root;

        const Groove& g = kGrooves[std::min(std::max(getInt(kGroove), 0), 4)];
        for (int v = 0; v < kPercVoices; ++v)
        {
            lumina::Voice nv;
            nv.channel = 3 + v; nv.rootNote = 60; nv.gateLength = 0.4f;
            nv.lanes[lumina::Voice::Gate] = lumina::Lane::gate(parsePat(g.pat[v]), 0.25);
            nv.lanes[lumina::Voice::Velocity] = lumina::Lane::constant(v == 3 ? 0.55f : 0.85f);
            if (g.pat[v][0] == 0) nv.muted = true;
            voices_[(size_t)v] = nv;
        }
    }

    // events -> voices
    void handle(const lumina::MidiEvent& e)
    {
        const int ch = (e.status & 0x0F) + 1, type = e.status & 0xF0;
        if (type == 0xB0 && e.data1 == 10) { const float p = ((float)e.data2 - 64.0f) / 64.0f; if (ch == 1) panA_ = p; else if (ch == 2) panB_ = p; return; }
        if (type != 0x90 || e.data2 == 0) return;
        const float vel = (float)e.data2 / 127.0f;
        int note = e.data1;
        if (ch == 1 || ch == 2)                      // snap down to the nearest tone of the mode (relative to the key)
            for (int k = 0; k < 12; ++k) { const int pc = ((note - root_ - k) % 12 + 12) % 12; if (modeMask_ & (1u << pc)) { note -= k; break; } }
        Hit* h = nullptr;
        for (auto& x : hits_) if (!x.active) { h = &x; break; }
        if (!h) h = &hits_[(size_t)(steal_++ % (unsigned)kPool)];
        *h = Hit{};
        h->active = true; h->gain = vel;
        const float sr = (float)sr_;
        if (ch == 1 || ch == 2)
        {
            const float hz = 440.0f * std::pow(2.0f, ((float)note - 69.0f) / 12.0f);
            const float dec = get(kDecay) / 100.0f;                              // 0..1
            const float secs = (ch == 2 ? 0.25f : 0.12f) + dec * (ch == 2 ? 0.9f : 0.8f);
            h->kind = 0; h->inc = hz / sr; h->coef = std::pow(0.001f, 1.0f / (secs * sr));
            h->amp = get(kArmLevel) / 100.0f * (ch == 2 ? 1.15f : 0.8f);
            const float pan = ch == 1 ? panA_ : panB_;
            h->l = std::cos((pan + 1.0f) * 0.78539816f); h->r = std::sin((pan + 1.0f) * 0.78539816f);
        }
        else
        {
            h->l = h->r = 0.7071f; h->amp = get(kPercLevel) / 100.0f;
            if (ch == 3)      { h->kind = 1; h->baseInc = 52.0f / sr; h->inc = 160.0f / sr; h->pitchEnv = 1.0f; h->pitchCoef = std::pow(0.001f, 1.0f / (0.07f * sr)); h->coef = std::pow(0.001f, 1.0f / (0.28f * sr)); h->amp *= 1.1f; }
            else if (ch == 4) { h->kind = 2; h->inc = 2500.0f / sr; h->coef = std::pow(0.001f, 1.0f / (0.05f * sr)); h->l = 0.9f; h->r = 0.45f; }
            else if (ch == 5) { h->kind = 3; h->baseInc = 190.0f / sr; h->inc = 300.0f / sr; h->pitchEnv = 1.0f; h->pitchCoef = std::pow(0.001f, 1.0f / (0.05f * sr)); h->coef = std::pow(0.001f, 1.0f / (0.16f * sr)); h->l = 0.45f; h->r = 0.9f; }
            else              { h->kind = 4; h->coef = std::pow(0.001f, 1.0f / (0.06f * sr)); h->amp *= 0.55f; }
        }
    }

    void render(mpc::ProcessContext& ctx, int from, int to, float gOut)
    {
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        for (int i = from; i < to; ++i)
        {
            float sl = 0, sr = 0;
            for (auto& h : hits_)
            {
                if (!h.active) continue;
                float s;
                switch (h.kind)
                {
                    case 0: { const float p = h.phase * kTwoPi; s = std::sin(p) + 0.35f * std::sin(2.0f * p) + 0.12f * std::sin(3.0f * p); h.phase += h.inc; if (h.phase >= 1.0f) h.phase -= 1.0f; break; }
                    case 1: case 3: { s = std::sin(h.phase * kTwoPi); const float f = h.baseInc + (h.inc - h.baseInc) * h.pitchEnv; h.phase += f; if (h.phase >= 1.0f) h.phase -= 1.0f; h.pitchEnv *= h.pitchCoef; break; }
                    case 2: { s = std::sin(h.phase * kTwoPi) * 0.8f + std::sin(h.phase * kTwoPi * 1.52f) * 0.3f; h.phase += h.inc; if (h.phase >= 1.0f) h.phase -= 1.0f; break; }
                    default: { const float n = noise_.process(); h.lp += 0.45f * (n - h.lp); s = n - h.lp; break; }   // high-passed noise
                }
                const float a = s * h.amp * h.gain;
                sl += a * h.l; sr += a * h.r;
                h.amp *= h.coef;
                if (h.amp < 0.0005f) h.active = false;
            }
            L[i] = softClip(sl * gOut * 0.5f);
            R[i] = softClip(sr * gOut * 0.5f);
        }
    }

    static float softClip(float x) { return x / (1.0f + std::fabs(x)); }

    double sr_ = 44100.0, free_ppq_ = 0.0;
    float panA_ = 0.0f, panB_ = 0.0f;
    bool rebuilt_ = false;
    float last_[kPlayhead] = {};
    // the live playhead: process() writes the picture number, this thread reports changes to the host (about every 100 ms)
    std::atomic<int> frame_ { 0 };
    std::atomic<bool> run_ { false };
    std::thread poll_;
    void pollLoop()
    {
        int last = -1;
        while (run_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const int f = frame_.load(std::memory_order_relaxed);
            if (f != last) { last = f; set(kPlayhead, static_cast<float>(f)); notifyHostParameterChanged(kPlayhead); }
        }
    }
    unsigned steal_ = 0, modeMask_ = 0x5AD;   // A-minor default pattern until the first rebuild
    int root_ = 9;
    lumina::Scheduler sched_;
    lumina::FibonacciVoice fib_;
    std::array<lumina::Voice, kPercVoices> voices_{};
    lumina::EventBuffer<256> ev_;
    std::array<Hit, kPool> hits_{};
    WhiteNoise noise_{0xC0FFEEu};
};

MPC_PLUGIN_ENTRY(Sunflower)
