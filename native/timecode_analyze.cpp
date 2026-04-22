// Host-side analyzer for the timecode decoder. Two modes:
//
//   timecode_analyze <file.wav> [hop_ms]
//       Stream the whole WAV through the decoder, print a speed / locked /
//       position time series (one line per hop_ms, default 250).
//
//   timecode_analyze <file.wav> seek <offset_sec> [duration_sec]
//       Seek into the WAV, decode a window, report the decoded position
//       vs. ground truth (offset_sec × resolutionHz).
//
// The seek mode is the validation harness: since the WAV is a linear
// recording of the Serato CD, byte offset maps 1:1 to timecode position.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "timecode.h"

namespace {

struct WavInfo {
    uint32_t sampleRate;
    uint16_t channels;
    std::vector<int16_t> samples;
};

uint32_t rd_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool readWav(const char* path, WavInfo& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "open %s failed\n", path); return false; }

    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0
        || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fprintf(stderr, "not a RIFF/WAVE file\n");
        std::fclose(f); return false;
    }

    bool gotFmt = false, gotData = false;
    uint16_t fmtTag = 0, bits = 0;
    while (!gotData) {
        uint8_t ck[8];
        if (std::fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = rd_u32_le(ck + 4);
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            std::vector<uint8_t> buf(sz);
            if (std::fread(buf.data(), 1, sz, f) != sz) break;
            fmtTag         = rd_u16_le(buf.data() + 0);
            out.channels   = rd_u16_le(buf.data() + 2);
            out.sampleRate = rd_u32_le(buf.data() + 4);
            bits           = rd_u16_le(buf.data() + 14);
            gotFmt = true;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            if (!gotFmt || fmtTag != 1 || bits != 16 || out.channels != 2) {
                std::fprintf(stderr,
                    "unsupported WAV (need PCM16 stereo; got tag=%u bits=%u ch=%u)\n",
                    fmtTag, bits, out.channels);
                std::fclose(f); return false;
            }
            out.samples.resize(sz / sizeof(int16_t));
            if (std::fread(out.samples.data(), 1, sz, f) != sz) {
                std::fprintf(stderr, "short read on data chunk\n");
                std::fclose(f); return false;
            }
            gotData = true;
        } else {
            std::fseek(f, sz, SEEK_CUR);
        }
    }
    std::fclose(f);
    return gotData;
}

int runStream(const WavInfo& wav, int hopMs) {
    timecode::Decoder dec;
    dec.begin((int)wav.sampleRate, timecode::Format::SeratoControlCD);

    const int totalFrames = (int)(wav.samples.size() / 2);
    const int hopFrames   = (int)((int64_t)hopMs * wav.sampleRate / 1000);
    if (hopFrames <= 0) { std::fprintf(stderr, "hop_ms too small\n"); return 1; }

    std::fprintf(stderr, "stream mode: hop %d ms, %d frames (%.1f s)\n",
                 hopMs, totalFrames, totalFrames / (double)wav.sampleRate);

    std::printf("   time(s)   speed  locked   position(cycles)  pos(s)\n");
    int frame = 0;
    while (frame < totalFrames) {
        int chunk = hopFrames;
        if (frame + chunk > totalFrames) chunk = totalFrames - frame;
        dec.pushFrames(wav.samples.data() + frame * 2, chunk);
        frame += chunk;
        const int32_t p = dec.position();
        const double  tSec = frame / (double)wav.sampleRate;
        const double  posSec = (p < 0) ? -1.0 : p / (double)dec.resolutionHz();
        std::printf("  %8.3f  %+6.3f    %d    %10d       %8.3f\n",
                    tSec, (double)dec.speed(), dec.locked() ? 1 : 0,
                    p, posSec);
    }
    return 0;
}

int runSeek(const WavInfo& wav, double offsetSec, double durSec) {
    timecode::Decoder dec;
    dec.begin((int)wav.sampleRate, timecode::Format::SeratoControlCD);

    const int totalFrames = (int)(wav.samples.size() / 2);
    int startFrame = (int)(offsetSec * wav.sampleRate);
    if (startFrame < 0 || startFrame >= totalFrames) {
        std::fprintf(stderr, "offset %.3f s out of range (file is %.3f s)\n",
                     offsetSec, totalFrames / (double)wav.sampleRate);
        return 1;
    }
    int windowFrames = (int)(durSec * wav.sampleRate);
    if (startFrame + windowFrames > totalFrames)
        windowFrames = totalFrames - startFrame;

    std::fprintf(stderr, "seek mode: offset %.3f s, duration %.3f s (%d frames)\n",
                 offsetSec, durSec, windowFrames);

    // Feed the window in smaller chunks so we can see how quickly lock acquires.
    const int chunk = (int)wav.sampleRate / 100;  // 10 ms
    const int16_t* p = wav.samples.data() + startFrame * 2;
    int fed = 0;
    int firstLockFrame = -1;
    int32_t lastReportedPos = -1;
    while (fed < windowFrames) {
        int c = chunk;
        if (fed + c > windowFrames) c = windowFrames - fed;
        dec.pushFrames(p, c);
        p   += c * 2;
        fed += c;
        if (firstLockFrame < 0 && dec.position() >= 0) firstLockFrame = fed;
        if (dec.position() >= 0) lastReportedPos = dec.position();
    }

    const int32_t decoded       = lastReportedPos;
    const double  decodedSec    = (decoded < 0) ? -1.0
                                  : decoded / (double)dec.resolutionHz();
    const double  expectedCycles = offsetSec * dec.resolutionHz();

    std::printf("\nresult:\n");
    std::printf("  expected pos  : %10.1f cycles  (= %.3f s at %d Hz)\n",
                expectedCycles, offsetSec, dec.resolutionHz());
    std::printf("  decoded  pos  : %10d cycles  (= %.3f s)\n",
                decoded, decodedSec);
    if (decoded >= 0) {
        double errCycles = decoded - expectedCycles;
        std::printf("  error         : %+.1f cycles (%+.3f s)\n",
                    errCycles, errCycles / dec.resolutionHz());
    }
    std::printf("  speed         : %+6.3f\n", (double)dec.speed());
    std::printf("  locked        : %d\n",     dec.locked() ? 1 : 0);
    std::printf("  first lock    : after %s\n",
                firstLockFrame < 0 ? "(never)"
                : (std::to_string(firstLockFrame * 1000 / (int)wav.sampleRate) + " ms").c_str());
    return (decoded >= 0) ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage:\n"
            "  %s <file.wav> [hop_ms]\n"
            "  %s <file.wav> seek <offset_sec> [duration_sec]\n",
            argv[0], argv[0]);
        return 1;
    }

    WavInfo wav;
    if (!readWav(argv[1], wav)) return 1;
    std::fprintf(stderr, "input: %u Hz stereo, %zu frames (%.1f s)\n",
                 wav.sampleRate, wav.samples.size() / 2,
                 (wav.samples.size() / 2) / (double)wav.sampleRate);

    if (argc >= 3 && std::strcmp(argv[2], "seek") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "seek mode needs <offset_sec>\n");
            return 1;
        }
        double off = std::atof(argv[3]);
        double dur = (argc >= 5) ? std::atof(argv[4]) : 2.0;
        return runSeek(wav, off, dur);
    }

    int hopMs = (argc >= 3) ? std::atoi(argv[2]) : 250;
    return runStream(wav, hopMs);
}
