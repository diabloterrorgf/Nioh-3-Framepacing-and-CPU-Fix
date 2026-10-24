// Fix E: which pacing rates auto mode may use. Pure functions (no globals), so they can be tested alone.
#pragma once
#include <cmath>

// Rates that divide the game's 120 Hz tick: every frame then shows the same number of ticks.
static const int kRates[] = { 120, 60, 40, 30 };
static const int kNumRates = 4;

// True when one frame at `rate` lasts a whole number of display refreshes (hz <= 1 = unknown).
// Then, without VRR, every frame is also shown for the same time (e.g. 30 fps on 60 Hz = 2 refreshes).
inline bool RateFitsDisplay(int rate, int hz) {
    if (hz <= 1) return true;
    double k = (double)hz / rate, n = std::floor(k + 0.5);
    return n >= 1 && std::fabs(k - n) < 0.05;     // 0.05 also accepts 59 Hz (59.94) and 119/143 Hz
}

// Rate index i may be used: fitting is off, the display is unknown, no rate fits it (e.g. 144 Hz,
// 165 Hz), or this rate fits it.
inline bool RateAllowed(int i, int hz, bool fit) {
    if (!fit || hz <= 1) return true;
    bool any = false;
    for (int j = 0; j < kNumRates; j++) any = any || RateFitsDisplay(kRates[j], hz);
    return !any || RateFitsDisplay(kRates[i], hz);
}

// Next allowed faster / slower rate index, or -1.
inline int FasterRate(int i, int hz, bool fit) {
    for (int j = i - 1; j >= 0; j--) if (RateAllowed(j, hz, fit)) return j;
    return -1;
}
inline int SlowerRate(int i, int hz, bool fit) {
    for (int j = i + 1; j < kNumRates; j++) if (RateAllowed(j, hz, fit)) return j;
    return -1;
}

// Fastest allowed rate not above fps; the slowest allowed rate if fps is below all of them.
inline int RateForFps(double fps, int hz, bool fit) {
    int last = kNumRates - 1;
    for (int i = 0; i < kNumRates; i++) {
        if (!RateAllowed(i, hz, fit)) continue;
        last = i;
        if (kRates[i] <= fps) return i;
    }
    return last;
}
