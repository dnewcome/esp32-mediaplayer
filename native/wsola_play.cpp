// Host-side harness for exercising the Wsola DSP against a WAV file.
//
//   wsola_play <input.wav> [speed]
//
// Speed defaults to 1.0. Raw interleaved S16LE stereo PCM is written to
// stdout at the input's sample rate. Pipe to aplay to hear the result:
//
//   ./wsola_play song.wav 1.25 | aplay -f S16_LE -c 2 -r 44100
//
// The WAV reader is deliberately minimal — PCM16, mono or stereo, no
// extensible headers, no floats. Anything fancier should be converted
// with ffmpeg first (`ffmpeg -i in.mp3 -f wav -acodec pcm_s16le out.wav`).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wsola.h"

namespace {

struct WavInfo {
    uint32_t sampleRate;
    uint16_t channels;
    std::vector<int16_t> samples;  // interleaved
};

uint32_t rd_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Parse PCM16 WAV. Returns false on any format we don't handle.
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
            fmtTag        = rd_u16_le(buf.data() + 0);
            out.channels  = rd_u16_le(buf.data() + 2);
            out.sampleRate = rd_u32_le(buf.data() + 4);
            bits          = rd_u16_le(buf.data() + 14);
            gotFmt = true;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            if (!gotFmt || fmtTag != 1 || bits != 16
                || (out.channels != 1 && out.channels != 2)) {
                std::fprintf(stderr,
                    "unsupported WAV (need PCM16 mono/stereo; got tag=%u bits=%u ch=%u)\n",
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
            // Skip unknown chunk (LIST, bext, etc).
            std::fseek(f, sz, SEEK_CUR);
        }
    }
    std::fclose(f);
    return gotData;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <input.wav> [speed]\n"
            "  pipe to: aplay -f S16_LE -c <channels> -r <rate>\n",
            argv[0]);
        return 1;
    }

    WavInfo wav;
    if (!readWav(argv[1], wav)) return 1;

    float speed = (argc >= 3) ? std::atof(argv[2]) : 1.0f;

    std::fprintf(stderr, "input: %u Hz, %u ch, %zu frames — speed %.3fx\n",
                 wav.sampleRate, wav.channels,
                 wav.samples.size() / wav.channels, speed);
    std::fprintf(stderr, "aplay command:\n"
                 "  aplay -f S16_LE -c 2 -r %u\n", wav.sampleRate);

    wsola::Wsola dsp;
    dsp.begin(wav.channels);
    dsp.setSpeed(speed);

    const int channels       = wav.channels;
    const int totalFrames    = (int)(wav.samples.size() / channels);
    const int16_t* inPtr     = wav.samples.data();
    int inFramesRemaining    = totalFrames;

    // Pull in chunks that match Wsola's internal scheduling.
    constexpr int OUT_CHUNK = 256;
    int16_t outBuf[OUT_CHUNK * 2];  // Wsola always emits stereo

    while (inFramesRemaining > 0 || dsp.outputAvailable() > 0) {
        // Push as much as will fit.
        int cap = dsp.inputCapacity();
        if (cap > inFramesRemaining) cap = inFramesRemaining;
        if (cap > 0) {
            int pushed = dsp.pushFrames(inPtr, cap);
            inPtr              += pushed * channels;
            inFramesRemaining  -= pushed;
        }

        // Drain everything currently producible.
        int got = dsp.pullFrames(outBuf, OUT_CHUNK);
        if (got <= 0 && inFramesRemaining == 0) break;
        if (got > 0) {
            std::fwrite(outBuf, sizeof(int16_t), (size_t)got * 2, stdout);
        }
    }
    return 0;
}
