# Mini Space Shooter — build system
#
# Design goals that shaped these flags:
#   * two languages: C11 for the platform/UI/audio layer, C++17 for the game + ML
#   * no STL containers, no exceptions, no RTTI anywhere in the codebase
#     => -fno-exceptions -fno-rtti, fixed-size static pools, predictable RAM
#   * only link-time dependencies: libX11, libXext, libm (audio is dlopen'd) 
#
# Targets: all (default) | run | test | shots | clean | help

CC        ?= cc
CXX       ?= c++
OPT       ?= -O2
BUILD     ?= build

BIN       := $(BUILD)/mini-space-shooter
TESTBIN   := $(BUILD)/selftest

SRC_C     := $(sort $(wildcard src/c/*.c))
SRC_CPP   := $(sort $(wildcard src/cpp/*.cpp))
SRC_TEST  := $(sort $(wildcard tests/*.cpp))
TESTOBJ   := $(patsubst tests/%.cpp,$(BUILD)/obj/tests/%.cpp.o,$(SRC_TEST))

OBJ       := $(patsubst src/%.c,$(BUILD)/obj/%.c.o,$(SRC_C)) \
             $(patsubst src/%.cpp,$(BUILD)/obj/%.cpp.o,$(SRC_CPP))

WARN_C    := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
             -Wpointer-arith -Wwrite-strings -Wvla -Wcast-qual
WARN_CPP  := -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual \
             -Wpointer-arith -Wcast-qual -Wuseless-cast

CFLAGS    := -std=c11 $(WARN_C) $(OPT) -pthread -MMD -MP -Isrc/c
CXXFLAGS  := -std=c++17 $(WARN_CPP) $(OPT) -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -MMD -MP -Isrc/c -Isrc/cpp
LDLIBS    := -lX11 -lXext -lm -ldl -pthread

.PHONY: all run test shots clean help strict sanitize window-test audio-test

all: $(BIN) $(TESTBIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(TESTBIN): $(TESTOBJ) $(filter-out $(BUILD)/obj/cpp/main.cpp.o,$(OBJ))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.cpp.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/tests/%.cpp.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

test: $(TESTBIN)
	./$(TESTBIN)

shots: $(BIN)
	./$(BIN) --shot out/shots

# Same as `all` but treats every warning as an error.
strict:
	$(MAKE) BUILD=$(BUILD)/strict OPT="-O2 -Werror" all test

sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize OPT="-O1 -g -Werror -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer" all test

$(BUILD)/window-smoke: tests/window_smoke.c $(BUILD)/obj/c/platform_x11.c.o $(BUILD)/obj/c/platform_sdl.c.o
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.o,$^) $(LDLIBS)

$(BUILD)/audio-smoke: tests/audio_smoke.c $(BUILD)/obj/c/audio.c.o
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.o,$^) $(LDLIBS)

audio-test: $(BUILD)/audio-smoke
	./$(BUILD)/audio-smoke

window-test: $(BUILD)/window-smoke
	./$(BUILD)/window-smoke

clean:
	rm -rf $(BUILD)

help:
	@echo "make            build game + selftest  -> $(BIN)"
	@echo "make run        build and launch the game"
	@echo "make test       build and run headless self-tests"
	@echo "make shots      render every screen to out/shots/*.ppm"
	@echo "make strict     rebuild with -Werror"
	@echo "make sanitize   run tests with address and undefined-behavior sanitizers"
	@echo "make window-test test the real display (no save changes)"
	@echo "make audio-test  test real playback and audio controls"
	@echo "make clean      remove $(BUILD)/"
	@echo
	@echo "Or simply: ./run.sh [run|test|shots|clean]"

-include $(OBJ:.o=.d) $(TESTOBJ:.o=.d)
-include $(BUILD)/window-smoke.d $(BUILD)/audio-smoke.d
