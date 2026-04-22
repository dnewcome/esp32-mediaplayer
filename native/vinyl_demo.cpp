// End-to-end demo of the timecode-control loop, host-side.
//
//   vinyl_demo <timecode.wav> <music.wav> <tc_speed>
//
// Simulates "spinning the platter faster/slower" without hardware: the
// timecode WAV is linearly resampled at tc_speed (varispeed — its pitch
// shifts too), fed to `timecode::Decoder`, and the decoded speed drives
// `wsola::Wsola` on the music track. Music comes out keylocked — faster
// or slower, same pitch — exactly what the firmware will do live.
//
// Stereo PCM16 at the music's sample rate is written to stdout:
//
//   ./vinyl_demo timecode.wav music.wav 1.25 | aplay -f S16_LE -c 2 -r 44100
//
// Both WAVs should share a sample rate (44.1 kHz in practice). Before
// the decoder locks (~160 ms of valid bits + EMA settling), music plays
// at the commanded tc_speed as a fallback so the demo doesn't stall.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "timecode.h"
#include "wsola.h"

namespace {

struct WavInfo {
    uint32_t sampleRate = 0;
    uint16_t channels   = 0;
    std::vector<int16_t> samples;  // interleaved
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
        std::fprintf(stderr, "%s: not RIFF/WAVE\n", path);
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
            if (!gotFmt || fmtTag != 1 || bits != 16
                || (out.channels != 1 && out.channels != 2)) {
                std::fprintf(stderr,
                    "%s: unsupported WAV (need PCM16 mono/stereo)\n", path);
                std::fclose(f); return false;
            }
            out.samples.resize(sz / sizeof(int16_t));
            if (std::fread(out.samples.data(), 1, sz, f) != sz) {
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

// Linear resample of interleaved-stereo TC at `speed` into out[CHUNK*2].
// Advances `pos` (fractional source index). Returns frames actually
// produced; 0 when we've run off the end of the source.
int resampleTc(const WavInfo& tc, double& pos, float speed,
               int16_t* out, int chunk) {
    const int nFrames = (int)(tc.samples.size() / tc.channels);
    const int ch      = tc.channels;
    int produced = 0;
    for (int i = 0; i < chunk; ++i) {
        int idx = (int)pos;
        if (idx >= nFrames - 1) break;
        double frac = pos - idx;
        auto sampleAt = [&](int f, int c) -> int16_t {
            if (ch == 1) return tc.samples[f];
            return tc.samples[f * 2 + c];
        };
        int16_t l0 = sampleAt(idx,     0), r0 = sampleAt(idx,     1);
        int16_t l1 = sampleAt(idx + 1, 0), r1 = sampleAt(idx + 1, 1);
        out[i * 2]     = (int16_t)(l0 + frac * (l1 - l0));
        out[i * 2 + 1] = (int16_t)(r0 + frac * (r1 - r0));
        pos += speed;
        ++produced;
    }
    return produced;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s <timecode.wav> <music.wav> <tc_speed>\n"
            "  Pipe stdout to: aplay -f S16_LE -c 2 -r <music-rate>\n",
            argv[0]);
        return 1;
    }
    WavInfo tc, music;
    if (!readWav(argv[1], tc))    return 1;
    if (!readWav(argv[2], music)) return 1;
    const float tcSpeed = (float)std::atof(argv[3]);

    if (tc.sampleRate != music.sampleRate) {
        std::fprintf(stderr,
            "sample-rate mismatch: tc=%u music=%u — resample one side first\n",
            tc.sampleRate, music.sampleRate);
        return 1;
    }
    if (music.channels != 2) {
        std::fprintf(stderr, "music must be stereo\n");
        return 1;
    }

    std::fprintf(stderr,
        "vinyl_demo: tc=%.1fs  music=%.1fs  cmd_speed=%.3fx  sr=%u\n",
        (tc.samples.size()    / tc.channels)    / (double)tc.sampleRate,
        (music.samples.size() / music.channels) / (double)music.sampleRate,
        tcSpeed, music.sampleRate);
    std::fprintf(stderr, "  play with: aplay -f S16_LE -c 2 -r %u\n",
                 music.sampleRate);

    timecode::Decoder dec;
    dec.begin((int)tc.sampleRate, timecode::Format::SeratoControlCD);

    wsola::Wsola wsola;
    wsola.begin(music.channels);
    wsola.setSpeed(tcSpeed);   // before lock: use commanded speed

    constexpr int CHUNK = 256;
    int16_t tcChunk[CHUNK * 2];
    int16_t outChunk[CHUNK * 2];

    double        tcPos         = 0.0;
    const int     musicFrames   = (int)(music.samples.size() / music.channels);
    int           musicIdx      = 0;   // frames consumed from music source
    int           outFrameCount = 0;
    int           lastReportFr  = 0;

    while (true) {
        // 1. Produce a chunk of resampled TC and feed the decoder.
        int tcGot = resampleTc(tc, tcPos, tcSpeed, tcChunk, CHUNK);
        if (tcGot == 0) break;
        dec.pushFrames(tcChunk, tcGot);

        // 2. Choose WSOLA speed from the decoder, falling back to the
        //    commanded speed until it locks so the demo starts audibly.
        float musicSpeed = dec.locked() ? dec.speed() : tcSpeed;
        if (musicSpeed < 0.25f) musicSpeed = 0.25f;
        if (musicSpeed > 4.0f)  musicSpeed = 4.0f;
        wsola.setSpeed(musicSpeed);

        // 3. Feed WSOLA enough music source to cover this output chunk.
        //    Keep pushing until either WSOLA can produce `tcGot` frames,
        //    or we've exhausted the music source.
        while (wsola.outputAvailable() < tcGot) {
            int cap   = wsola.inputCapacity();
            int avail = musicFrames - musicIdx;
            if (cap <= 0 || avail <= 0) break;
            int push   = std::min(cap, avail);
            int pushed = wsola.pushFrames(
                &music.samples[musicIdx * music.channels], push);
            if (pushed == 0) break;
            musicIdx += pushed;
        }

        int got = wsola.pullFrames(outChunk, tcGot);
        if (got <= 0 && musicIdx >= musicFrames) break;
        if (got > 0) {
            std::fwrite(outChunk, sizeof(int16_t), (size_t)got * 2, stdout);
            outFrameCount += got;
        }

        // 4. Status line once per second of output.
        if (outFrameCount - lastReportFr >= (int)music.sampleRate) {
            std::fprintf(stderr,
                "t=%5.1fs  cmd=%.2fx  decoded=%+.2fx  locked=%d  music_used=%.1fs\n",
                outFrameCount / (double)music.sampleRate,
                tcSpeed, (double)dec.speed(), dec.locked() ? 1 : 0,
                musicIdx / (double)music.sampleRate);
            lastReportFr = outFrameCount;
        }
    }
    return 0;
}
