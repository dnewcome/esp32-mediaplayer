# Host builds for DSP + full SDL-based player harness.
#
# Build:
#   make                # build everything under native/
#   make wsola_play     # just the CLI DSP tool
#   make mediaplayer    # just the SDL GUI player
#   make timecode_test  # synthetic-carrier smoke test
#   make timecode_analyze
#
# Run:
#   make run_sim                     # SDL player against ./media  (alias: run)
#   make run_wokwi                   # Wokwi headless ESP32 UI sim
#   make run_play  WAV=foo.wav SPEED=1.25
#   make run_tc                      # synthetic timecode test
#   make run_tc_wav TC_WAV=path.wav  # decode a real timecode capture
#   make run_vinyl_demo DEMO_SPEED=1.25 MUSIC=path.wav
#                                    # end-to-end: TC drives WSOLA on music

CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -DWSOLA_NATIVE -DTIMECODE_NATIVE -I../src
LDFLAGS  ?=

SDL_CFLAGS = $(shell pkg-config --cflags sdl2 SDL2_ttf)
SDL_LIBS   = $(shell pkg-config --libs   sdl2 SDL2_ttf)

WSOLA_SRC    = ../src/wsola.cpp
TIMECODE_SRC = ../src/timecode.cpp

WAV   ?= input.wav
SPEED ?= 1.0

TC_WAV ?= ../timecode.wav
TC_HOP ?= 250

# vinyl_demo inputs. Override MUSIC=/path/to/song.wav as needed.
MUSIC    ?= media/bass.wav
DEMO_SPEED ?= 1.25

# Wokwi sim lives at the project root; these paths are relative to ../.
WOKWI_ENV      ?= esp32-sim
WOKWI_ELF      ?= .pio/build/$(WOKWI_ENV)/firmware.elf
WOKWI_SCENARIO ?= sim/scenario.test.yaml
WOKWI_PROJECT  ?= sim/

all: wsola_play mediaplayer timecode_test timecode_analyze vinyl_demo

wsola_play: wsola_play.cpp $(WSOLA_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

mediaplayer: mediaplayer.cpp $(WSOLA_SRC)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $^ -o $@ $(LDFLAGS) $(SDL_LIBS)

timecode_test: timecode_test.cpp $(TIMECODE_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

timecode_analyze: timecode_analyze.cpp $(TIMECODE_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

vinyl_demo: vinyl_demo.cpp $(TIMECODE_SRC) $(WSOLA_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

run_sim: mediaplayer
	./mediaplayer media

# Back-compat alias.
run: run_sim

# Build the esp32-sim PlatformIO env (always consult pio — it decides
# whether anything actually needs to rebuild).
sim_firmware:
	cd .. && pio run -e $(WOKWI_ENV)

# Headless Wokwi run. Loads WOKWI_CLI_TOKEN from ../.env; fails loudly
# if .env is missing rather than silently running unauthenticated.
run_wokwi: sim_firmware
	@test -f ../.env || { echo "missing ../.env with WOKWI_CLI_TOKEN"; exit 1; }
	cd .. && env $$(grep -v '^#' .env | xargs) \
	    wokwi-cli --elf $(WOKWI_ELF) \
	              --scenario $(WOKWI_SCENARIO) \
	              $(WOKWI_PROJECT)

run_play: wsola_play
	./wsola_play $(WAV) $(SPEED) | aplay -q -f S16_LE -c 2 -r 44100

run_tc: timecode_test
	./timecode_test

run_tc_wav: timecode_analyze
	./timecode_analyze $(TC_WAV) $(TC_HOP)

# End-to-end vinyl-control demo: TC WAV → decoder → WSOLA on music → aplay.
# Override: make run_vinyl_demo DEMO_SPEED=0.8 MUSIC=path/to/song.wav
run_vinyl_demo: vinyl_demo
	./vinyl_demo $(TC_WAV) $(MUSIC) $(DEMO_SPEED) | aplay -q -f S16_LE -c 2 -r 44100

clean:
	rm -f wsola_play mediaplayer timecode_test timecode_analyze vinyl_demo

.PHONY: all run run_sim run_wokwi sim_firmware run_play run_tc run_tc_wav run_vinyl_demo clean
