// A tiny REAL-TIME VST2 host for Sunflower's live playhead: loads the plugin DLL, plays a transport (120 bpm) paced by the wall clock, and records every
// audioMasterAutomate call the plugin makes, then checks the playhead against the transport.
#include "mpc_vst2_abi.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static clk::time_point t0;
static double nowS() { return std::chrono::duration<double>(clk::now() - t0).count(); }
struct Evt { double t; int idx; float val; };
static std::vector<Evt> evts; static std::mutex mu;
static VstTimeInfo ti; static std::atomic<int> g_updateDisplay { 0 }, g_playing { 1 };
static double g_ppq = 0.0;

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32 idx, VstIntPtr, void*, float opt) {
    if (op == audioMasterAutomate) { std::lock_guard<std::mutex> l(mu); evts.push_back({ nowS(), idx, opt }); return 0; }
    if (op == audioMasterUpdateDisplay) { ++g_updateDisplay; return 0; }
    if (op == audioMasterGetTime) {
        ti.ppqPos = g_ppq; ti.tempo = 120.0; ti.sampleRate = 48000.0;
        ti.flags = kVstTempoValid | kVstPpqPosValid | (g_playing.load() ? kVstTransportPlaying : 0);
        ti.timeSigNumerator = 4; ti.timeSigDenominator = 4;
        return (VstIntPtr)&ti;
    }
    if (op == audioMasterVersion) return 2400;
    return 0;
}

int main(int argc, char** argv) {
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) { std::printf("cannot load %s\n", argv[1]); return 2; }
    auto mainFn = (AEffect * (*)(audioMasterCallback)) GetProcAddress(h, "VSTPluginMain");
    AEffect* e = mainFn(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 48000.0f);
    e->dispatcher(e, effSetBlockSize, 0, 512, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    const int PH = e->numParams - 1;
    std::printf("parameters: %d; playhead index %d\n", e->numParams, PH);
    std::printf("effCanBeAutomated: playhead=%d, arms(1)=%d, output(0)=%d\n", (int)e->dispatcher(e, effCanBeAutomated, PH, 0, nullptr, 0), (int)e->dispatcher(e, effCanBeAutomated, 1, 0, nullptr, 0), (int)e->dispatcher(e, effCanBeAutomated, 0, 0, nullptr, 0));
    std::vector<float> L(512), R(512); float* out[2] = { L.data(), R.data() };
    const double blockSec = 512.0 / 48000.0, beatsPerBlock = blockSec * 2.0;   // 120 bpm = 2 beats a second
    t0 = clk::now();
    const double playSec = 13.0, stopSec = 1.0;
    double next = 0.0;
    while (nowS() < playSec + stopSec) {
        if (nowS() >= playSec) g_playing = 0;
        e->processReplacing(e, nullptr, out, 512);
        if (g_playing) g_ppq += beatsPerBlock;
        next += blockSec; const double wait = next - nowS(); if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    }
    // ---- analyse ----
    std::vector<Evt> v; { std::lock_guard<std::mutex> l(mu); v = evts; }
    int bad = 0, wraps = 0, nonmono = 0; double minGap = 1e9, maxGap = 0; int last = -1; double lastT = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].idx != PH) ++bad;
        const int f = (int)std::lround(v[i].val * 63.0);
        if (i) { const double g = v[i].t - lastT; minGap = std::min(minGap, g); maxGap = std::max(maxGap, g); if (f < last) ++wraps; }
        last = f; lastT = v[i].t;
    }
    std::printf("automate calls: %zu in %.0f s (%.1f per second), all for the playhead: %s; gaps min %.0f ms max %.0f ms; wraps %d; updateDisplay calls: %d\n",
                v.size(), playSec, v.size() / playSec, bad ? "NO" : "yes", minGap * 1000, maxGap * 1000, wraps, g_updateDisplay.load());
    // does the picture number follow the transport? expected frame at time t: floor(phase*64), phase = (t*2 mod 16)/16 (4 bars of 4/4 at 120 bpm = 8 s per cycle)
    int worst = 0; for (const Evt& q : v) { if (q.t > playSec) break; const double ph = std::fmod(q.t * 2.0, 16.0) / 16.0; int exp = (int)(ph * 64); int f = (int)std::lround(q.val * 63.0); int d = std::abs(f - exp); d = std::min(d, 64 - d); worst = std::max(worst, d); }
    std::printf("worst difference between the picture shown and where the transport really is: %d frame(s) = %.0f ms\n", worst, worst * 125.0);
    // after stop
    float fin = e->getParameter(e, PH);
    std::printf("after stopping: playhead value %.4f (frame %d); last automate at %.2f s (transport stopped at %.0f s)\n", fin, (int)std::lround(fin * 63.0), v.empty() ? 0.0 : v.back().t, playSec);
    for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i) std::printf("   t=%.3f  frame %2d\n", v[i].t, (int)std::lround(v[i].val * 63.0));
    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    return 0;
}
