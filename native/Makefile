# Host builds for DSP + full SDL-based player harness.
#
#   make              # builds wsola_play and mediaplayer
#   make wsola_play   # just the CLI DSP tool
#   make mediaplayer  # just the SDL GUI player
#   make run_play WAV=song.wav SPEED=1.25
#   make run          # launches the GUI against ./media

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

all: wsola_play mediaplayer timecode_test timecode_analyze

wsola_play: wsola_play.cpp $(WSOLA_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

mediaplayer: mediaplayer.cpp $(WSOLA_SRC)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $^ -o $@ $(LDFLAGS) $(SDL_LIBS)

timecode_test: timecode_test.cpp $(TIMECODE_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

timecode_analyze: timecode_analyze.cpp $(TIMECODE_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

run: mediaplayer
	./mediaplayer media

run_play: wsola_play
	./wsola_play $(WAV) $(SPEED) | aplay -q -f S16_LE -c 2 -r 44100

run_tc: timecode_test
	./timecode_test

run_tc_wav: timecode_analyze
	./timecode_analyze $(TC_WAV) $(TC_HOP)

clean:
	rm -f wsola_play mediaplayer timecode_test timecode_analyze

.PHONY: all run run_play run_tc run_tc_wav clean
