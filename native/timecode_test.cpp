// Host-side smoke test for the timecode Decoder. Generates synthetic stereo
// sine carriers at various frequencies and L/R phase relationships, feeds
// them into the decoder, and prints the steady-state speed estimate.
//
//   make timecode_test && ./timecode_test
//
// This exercises the zero-crossing + direction detection paths in
// src/timecode.cpp without needing real vinyl or codec hardware. The bit
// decoder and position lookup aren't implemented yet — that's a separate
// TODO that'll need real-vinyl captures to validate against.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "timecode.h"

namespace {

constexpr int    kSampleRate = 44100;
constexpr double kPi         = 3.14159265358979323846;

// Generate a stereo carrier: L = sin(2πft), R = sin(2πft + phase).
// phase = +π/2 (90°) is the canonical "forward" Serato convention (L leads R).
// phase = -π/2 reverses it.
std::vector<int16_t> makeCarrier(double freqHz, double phaseRad,
                                 double durationSec, int16_t amp = 8000) {
    int frames = (int)(durationSec * kSampleRate);
    std::vector<int16_t> out(frames * 2);
    const double step = 2.0 * kPi * freqHz / (double)kSampleRate;
    double t = 0.0;
    for (int i = 0; i < frames; ++i) {
        out[i * 2]     = (int16_t)(amp * std::sin(t));
        out[i * 2 + 1] = (int16_t)(amp * std::sin(t + phaseRad));
        t += step;
    }
    return out;
}

struct Scenario {
    const char* label;
    double      freqHz;
    double      phaseRad;
    float       expectedSpeedAbs;   // magnitude — sign checked separately
    int         expectedSign;       // +1, -1, or 0 for silence
};

void run(const Scenario& s) {
    timecode::Decoder dec;
    dec.begin(kSampleRate);

    auto sig = makeCarrier(s.freqHz, s.phaseRad, 0.5 /* sec */);
    dec.pushFrames(sig.data(), (int)(sig.size() / 2));

    const float speed = dec.speed();
    const bool  lock  = dec.locked();

    std::printf("  %-32s  f=%6.1fHz  phase=%+5.0f°  speed=%+6.3f  locked=%d\n",
                s.label, s.freqHz, s.phaseRad * 180.0 / kPi, (double)speed,
                lock ? 1 : 0);
}

void runSilence() {
    timecode::Decoder dec;
    dec.begin(kSampleRate);
    std::vector<int16_t> silence(kSampleRate * 2 * 2, 0);  // 1s stereo silence
    dec.pushFrames(silence.data(), kSampleRate);
    std::printf("  %-32s  %-28s  speed=%+6.3f  locked=%d\n",
                "silence", "", (double)dec.speed(), dec.locked() ? 1 : 0);
}

} // namespace

int main() {
    std::printf("timecode decoder smoke test\n");
    std::printf("  carrier reference: 1000 Hz @ 44.1 kHz → speed 1.0 expected\n\n");

    runSilence();

    const Scenario scenarios[] = {
        { "1000 Hz, L leads R (+90)",  1000.0,  kPi / 2, 1.0f, +1 },
        { "1000 Hz, R leads L (-90)",  1000.0, -kPi / 2, 1.0f, -1 },
        { " 500 Hz, forward (slow)",    500.0,  kPi / 2, 0.5f, +1 },
        { "1500 Hz, forward (fast)",   1500.0,  kPi / 2, 1.5f, +1 },
        { "2000 Hz, reverse",          2000.0, -kPi / 2, 2.0f, -1 },
    };
    for (auto& s : scenarios) run(s);

    return 0;
}
