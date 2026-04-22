// Host-side DJ-player harness. Re-implements the on-device I/O layer against
// SDL2 so the media player can be exercised without hardware: window for the
// OLED, keyboard for encoder/buttons, system audio for playback.
//
// Shared with firmware: src/wsola.cpp (compiled with -DWSOLA_NATIVE).
// Everything else here is a host-only stand-in.
//
// Controls:
//   Browser:  Up/Down scroll, Enter play selection
//   Playing:  Up/Down nudge speed ±0.02x, Enter snap to 1.0x
//             Space pause/resume, Backspace/Esc back to browser
//             K toggle keylock, C jump to cue (or restart), Shift+C set cue
//
// Media:  put PCM16 WAVs in ./media/ (or pass a directory as argv[1]).
//         `ffmpeg -i in.mp3 -acodec pcm_s16le -ar 44100 -ac 2 out.wav`

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "wsola.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// WAV reader (PCM16 mono/stereo) — same shape as wsola_play.cpp.
// ---------------------------------------------------------------------------

struct Wav {
    uint32_t              sampleRate = 0;
    uint16_t              channels   = 0;
    std::vector<int16_t>  samples;    // interleaved
};

uint32_t rd_u32(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|(uint32_t(p[3])<<24); }
uint16_t rd_u16(const uint8_t* p) { return p[0]|(p[1]<<8); }

bool loadWav(const std::string& path, Wav& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[12];
    if (std::fread(hdr,1,12,f)!=12 || std::memcmp(hdr,"RIFF",4) || std::memcmp(hdr+8,"WAVE",4)) {
        std::fclose(f); return false;
    }
    bool gotFmt=false, gotData=false;
    uint16_t fmtTag=0, bits=0;
    while (!gotData) {
        uint8_t ck[8];
        if (std::fread(ck,1,8,f)!=8) break;
        uint32_t sz = rd_u32(ck+4);
        if (!std::memcmp(ck,"fmt ",4)) {
            std::vector<uint8_t> b(sz);
            if (std::fread(b.data(),1,sz,f)!=sz) break;
            fmtTag      = rd_u16(b.data()+0);
            out.channels = rd_u16(b.data()+2);
            out.sampleRate = rd_u32(b.data()+4);
            bits        = rd_u16(b.data()+14);
            gotFmt = true;
        } else if (!std::memcmp(ck,"data",4)) {
            if (!gotFmt || fmtTag!=1 || bits!=16 || (out.channels!=1 && out.channels!=2)) {
                std::fclose(f); return false;
            }
            out.samples.resize(sz/sizeof(int16_t));
            if (std::fread(out.samples.data(),1,sz,f)!=sz) { std::fclose(f); return false; }
            gotData = true;
        } else {
            std::fseek(f, sz, SEEK_CUR);
        }
    }
    std::fclose(f);
    return gotData;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

constexpr int kWinW = 640;
constexpr int kWinH = 360;
constexpr int kSampleRate = 44100;
constexpr int kOutChannels = 2;
constexpr int kAudioTargetQueuedMs = 200;
constexpr float kSpeedStep = 0.02f;
constexpr float kSpeedMin  = 0.5f;
constexpr float kSpeedMax  = 2.0f;

enum class Screen { Browser, Playing };

struct App {
    SDL_Window*       win = nullptr;
    SDL_Renderer*     ren = nullptr;
    TTF_Font*         fontLg = nullptr;   // 22pt — headers, track name
    TTF_Font*         fontMd = nullptr;   // 16pt — list rows, speed
    TTF_Font*         fontSm = nullptr;   // 12pt — footer / hints
    SDL_AudioDeviceID dev   = 0;

    // library
    std::vector<std::string> files;   // just filenames
    std::string              mediaDir;
    int                      selected = 0;

    // playback
    Screen      screen    = Screen::Browser;
    int         loaded    = -1;       // index of currently-loaded WAV
    Wav         wav;
    size_t      readFrame = 0;
    float       speed     = 1.0f;
    bool        paused    = false;
    bool        keylock   = false;
    bool        cueValid  = false;
    size_t      cueFrame  = 0;

    // DSP state
    wsola::Wsola dsp;
    double       pitchedPos = 0.0;      // fractional read index for pitched mode
};

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

void drawText(App& a, TTF_Font* font, const std::string& s, int x, int y,
              SDL_Color col = {255,255,255,255}) {
    if (s.empty()) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), col);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(a.ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(a.ren, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void drawBrowser(App& a) {
    SDL_SetRenderDrawColor(a.ren, 0, 0, 0, 255);
    SDL_RenderClear(a.ren);

    drawText(a, a.fontLg, "Files", 16, 12);
    SDL_SetRenderDrawColor(a.ren, 120, 120, 120, 255);
    SDL_RenderDrawLine(a.ren, 16, 48, kWinW - 16, 48);

    // Scroll window of 6 rows around the selected entry.
    constexpr int ROWS = 6;
    constexpr int ROW_H = 34;
    int start = a.selected - ROWS/2;
    int n = (int)a.files.size();
    if (start > n - ROWS) start = n - ROWS;
    if (start < 0) start = 0;

    for (int i = 0; i < ROWS && start + i < n; ++i) {
        int idx = start + i;
        int y = 60 + i * ROW_H;
        if (idx == a.selected) {
            SDL_Rect bar = { 12, y - 2, kWinW - 24, ROW_H - 4 };
            SDL_SetRenderDrawColor(a.ren, 255, 255, 255, 255);
            SDL_RenderFillRect(a.ren, &bar);
            drawText(a, a.fontMd, a.files[idx], 20, y + 2, {0,0,0,255});
        } else {
            drawText(a, a.fontMd, a.files[idx], 20, y + 2);
        }
    }

    if (a.files.empty()) {
        drawText(a, a.fontMd, "no .wav files in " + a.mediaDir, 16, 80);
    }

    drawText(a, a.fontSm,
        "Up/Down: scroll   Enter: play   Q: quit",
        16, kWinH - 22, {160,160,160,255});
}

void drawPlaying(App& a) {
    SDL_SetRenderDrawColor(a.ren, 0, 0, 0, 255);
    SDL_RenderClear(a.ren);

    drawText(a, a.fontLg, a.paused ? "Paused" : "Playing", 16, 12);
    std::string flags;
    if (a.keylock)  flags += "KEY ";
    if (a.cueValid) flags += "CUE";
    drawText(a, a.fontSm, flags, kWinW - 100, 20, {255,220,100,255});

    SDL_SetRenderDrawColor(a.ren, 120, 120, 120, 255);
    SDL_RenderDrawLine(a.ren, 16, 48, kWinW - 16, 48);

    drawText(a, a.fontMd,
        (a.loaded >= 0 && a.loaded < (int)a.files.size()) ? a.files[a.loaded] : "(none)",
        16, 64);

    char buf[48];
    std::snprintf(buf, sizeof(buf), "Speed: %.2fx", a.speed);
    drawText(a, a.fontMd, buf, 16, 110);

    // Pitch bar: frame + fill from center.
    constexpr int BAR_Y = 160, BAR_H = 18;
    int barX = 32, barW = kWinW - 64;
    int mid = barX + barW/2;
    SDL_Rect frame = { barX, BAR_Y, barW, BAR_H };
    SDL_SetRenderDrawColor(a.ren, 180, 180, 180, 255);
    SDL_RenderDrawRect(a.ren, &frame);
    int delta = (int)((a.speed - 1.0f) * (barW / 2) / (kSpeedMax - 1.0f));
    if (delta > barW/2) delta = barW/2;
    if (delta < -barW/2) delta = -barW/2;
    SDL_SetRenderDrawColor(a.ren, 100, 220, 140, 255);
    if (delta >= 0) {
        SDL_Rect bar = { mid, BAR_Y + 2, delta, BAR_H - 4 };
        SDL_RenderFillRect(a.ren, &bar);
    } else {
        SDL_Rect bar = { mid + delta, BAR_Y + 2, -delta, BAR_H - 4 };
        SDL_RenderFillRect(a.ren, &bar);
    }
    SDL_SetRenderDrawColor(a.ren, 255, 255, 255, 255);
    SDL_RenderDrawLine(a.ren, mid, BAR_Y - 4, mid, BAR_Y + BAR_H + 4);

    drawText(a, a.fontSm,
        "Up/Dn: speed   Enter: 1.0x   K: keylock   Space: pause   C: cue   Shift+C: set cue   Esc: back",
        16, kWinH - 22, {160,160,160,255});
}

// ---------------------------------------------------------------------------
// Audio — fills SDL queue with up to ~kAudioTargetQueuedMs of stereo S16LE.
// ---------------------------------------------------------------------------

int pullKeylockFrames(App& a, int16_t* out, int frames) {
    // Keep the WSOLA input fed, then drain.
    int produced = 0;
    while (produced < frames) {
        int cap = a.dsp.inputCapacity();
        if (cap > 0 && a.readFrame < a.wav.samples.size() / a.wav.channels) {
            size_t avail = a.wav.samples.size() / a.wav.channels - a.readFrame;
            int push = (int)std::min((size_t)cap, avail);
            a.dsp.pushFrames(a.wav.samples.data() + a.readFrame * a.wav.channels, push);
            a.readFrame += push;
        }
        int n = a.dsp.pullFrames(out + produced * 2, frames - produced);
        if (n > 0) { produced += n; continue; }
        if (a.readFrame >= a.wav.samples.size() / a.wav.channels) break;  // EOF + nothing more
    }
    return produced;
}

int pullPitchedFrames(App& a, int16_t* out, int frames) {
    // Naive linear-interpolating resampler. Step the read position by `speed`
    // per output frame; pitch tracks speed (varispeed turntable).
    const size_t totalFrames = a.wav.samples.size() / a.wav.channels;
    int produced = 0;
    const int ch = a.wav.channels;
    while (produced < frames) {
        size_t i0 = (size_t)a.pitchedPos;
        if (i0 + 1 >= totalFrames) break;
        float frac = (float)(a.pitchedPos - (double)i0);
        int16_t L, R;
        if (ch == 2) {
            int16_t a0 = a.wav.samples[i0 * 2], a1 = a.wav.samples[(i0+1) * 2];
            int16_t b0 = a.wav.samples[i0 * 2 + 1], b1 = a.wav.samples[(i0+1) * 2 + 1];
            L = (int16_t)(a0 + frac * (a1 - a0));
            R = (int16_t)(b0 + frac * (b1 - b0));
        } else {
            int16_t s0 = a.wav.samples[i0], s1 = a.wav.samples[i0+1];
            int16_t m  = (int16_t)(s0 + frac * (s1 - s0));
            L = R = m;
        }
        out[produced * 2]     = L;
        out[produced * 2 + 1] = R;
        ++produced;
        a.pitchedPos += (double)a.speed;
    }
    return produced;
}

void fillAudio(App& a) {
    if (a.loaded < 0 || a.paused) return;

    Uint32 queued = SDL_GetQueuedAudioSize(a.dev);
    const Uint32 target = (Uint32)(kSampleRate * kOutChannels * sizeof(int16_t)
                                   * kAudioTargetQueuedMs / 1000);
    if (queued >= target) return;

    constexpr int CHUNK = 512;  // frames per pull cycle
    int16_t buf[CHUNK * 2];
    while (SDL_GetQueuedAudioSize(a.dev) < target) {
        int n = a.keylock ? pullKeylockFrames(a, buf, CHUNK)
                          : pullPitchedFrames(a, buf, CHUNK);
        if (n <= 0) break;
        SDL_QueueAudio(a.dev, buf, n * 2 * sizeof(int16_t));
    }
}

// ---------------------------------------------------------------------------
// Track loading / control actions
// ---------------------------------------------------------------------------

void loadTrack(App& a, int idx) {
    if (idx < 0 || idx >= (int)a.files.size()) return;
    Wav w;
    if (!loadWav(a.mediaDir + "/" + a.files[idx], w)) {
        std::fprintf(stderr, "failed to load %s\n", a.files[idx].c_str());
        return;
    }
    a.wav        = std::move(w);
    a.loaded     = idx;
    a.readFrame  = 0;
    a.pitchedPos = 0.0;
    a.cueValid   = false;
    a.cueFrame   = 0;
    a.dsp.begin(a.wav.channels);
    a.dsp.setSpeed(a.speed);
    SDL_ClearQueuedAudio(a.dev);
}

void seekTo(App& a, size_t frame) {
    a.readFrame  = frame;
    a.pitchedPos = (double)frame;
    a.dsp.reset();
    a.dsp.setSpeed(a.speed);
    SDL_ClearQueuedAudio(a.dev);
}

void playSelected(App& a) {
    loadTrack(a, a.selected);
    a.screen = Screen::Playing;
    a.paused = false;
    SDL_PauseAudioDevice(a.dev, 0);
}

void adjustSpeed(App& a, float delta) {
    a.speed += delta;
    if (a.speed < kSpeedMin) a.speed = kSpeedMin;
    if (a.speed > kSpeedMax) a.speed = kSpeedMax;
    a.dsp.setSpeed(a.speed);
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void handleKey(App& a, SDL_Keycode k, Uint16 mod) {
    if (a.screen == Screen::Browser) {
        switch (k) {
            case SDLK_UP:     if (a.selected > 0) --a.selected; break;
            case SDLK_DOWN:   if (a.selected + 1 < (int)a.files.size()) ++a.selected; break;
            case SDLK_RETURN: playSelected(a); break;
            default: break;
        }
    } else {
        switch (k) {
            case SDLK_UP:       adjustSpeed(a,  kSpeedStep); break;
            case SDLK_DOWN:     adjustSpeed(a, -kSpeedStep); break;
            case SDLK_RETURN:   a.speed = 1.0f; a.dsp.setSpeed(1.0f); break;
            case SDLK_SPACE:    a.paused = !a.paused;
                                SDL_PauseAudioDevice(a.dev, a.paused ? 1 : 0); break;
            case SDLK_BACKSPACE:
            case SDLK_ESCAPE:   a.paused = true;
                                SDL_PauseAudioDevice(a.dev, 1);
                                SDL_ClearQueuedAudio(a.dev);
                                a.screen = Screen::Browser; break;
            case SDLK_k:        a.keylock = !a.keylock;
                                seekTo(a, a.readFrame); break;
            case SDLK_c:
                if (mod & KMOD_SHIFT) {
                    a.cueFrame = a.keylock ? a.readFrame : (size_t)a.pitchedPos;
                    a.cueValid = true;
                } else {
                    seekTo(a, a.cueValid ? a.cueFrame : 0);
                }
                break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------

void scanMedia(App& a) {
    a.files.clear();
    std::error_code ec;
    if (!fs::exists(a.mediaDir, ec)) {
        fs::create_directory(a.mediaDir, ec);
    }
    for (auto& entry : fs::directory_iterator(a.mediaDir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".wav") a.files.push_back(entry.path().filename().string());
    }
    std::sort(a.files.begin(), a.files.end());
}

bool initSdl(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false;
    }
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return false;
    }
    a.win = SDL_CreateWindow("esp32-mediaplayer (host)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWinW, kWinH, SDL_WINDOW_SHOWN);
    if (!a.win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    a.ren = SDL_CreateRenderer(a.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!a.ren) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }

    const char* fontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
    a.fontLg = TTF_OpenFont(fontPath, 22);
    a.fontMd = TTF_OpenFont(fontPath, 16);
    a.fontSm = TTF_OpenFont(fontPath, 12);
    if (!a.fontLg || !a.fontMd || !a.fontSm) {
        std::fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError()); return false;
    }

    SDL_AudioSpec want{}, have{};
    want.freq = kSampleRate;
    want.format = AUDIO_S16LSB;
    want.channels = kOutChannels;
    want.samples = 1024;
    a.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!a.dev) { std::fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }
    SDL_PauseAudioDevice(a.dev, 0);
    return true;
}

void teardown(App& a) {
    if (a.dev) SDL_CloseAudioDevice(a.dev);
    if (a.fontLg) TTF_CloseFont(a.fontLg);
    if (a.fontMd) TTF_CloseFont(a.fontMd);
    if (a.fontSm) TTF_CloseFont(a.fontSm);
    if (a.ren) SDL_DestroyRenderer(a.ren);
    if (a.win) SDL_DestroyWindow(a.win);
    TTF_Quit();
    SDL_Quit();
}

} // namespace

int main(int argc, char** argv) {
    App a;
    a.mediaDir = (argc >= 2) ? argv[1] : "media";

    if (!initSdl(a)) { teardown(a); return 1; }
    scanMedia(a);
    std::fprintf(stderr, "scanned %s — %zu wav file(s)\n", a.mediaDir.c_str(), a.files.size());

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.sym == SDLK_q) { running = false; break; }
                    handleKey(a, e.key.keysym.sym, e.key.keysym.mod);
                    break;
            }
        }

        if (a.screen == Screen::Playing) fillAudio(a);

        if (a.screen == Screen::Browser) drawBrowser(a);
        else                             drawPlaying(a);
        SDL_RenderPresent(a.ren);
    }

    teardown(a);
    return 0;
}
