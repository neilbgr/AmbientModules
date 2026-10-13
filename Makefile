RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk

# Host-side DSP unit tests (test/dsp_tests.cpp): compiled against src/dsp and
# Rack's headers only — no Rack engine, no libRack — then run. Same math
# flags as the plugin build (compile.mk), so the numbers match what ships.
#   make test RACK_DIR=/path/to/Rack-SDK
# Uses a native g++ when there is one; otherwise MinGW (static .exe, run
# through WSL's Windows interop).
TEST_CXX ?= $(if $(shell command -v g++ 2>/dev/null),g++,x86_64-w64-mingw32-g++)
TEST_FLAGS := -std=c++11 -O3 -funsafe-math-optimizations -march=nehalem -D_USE_MATH_DEFINES
TEST_FLAGS += -Wall -Wextra -Wno-unused-parameter -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -Isrc
ifneq (,$(findstring mingw,$(TEST_CXX)))
	TEST_FLAGS += -static
endif

.PHONY: test
test:
	@mkdir -p build/test
	$(TEST_CXX) $(TEST_FLAGS) test/dsp_tests.cpp -o build/test/dsp_tests.exe
	./build/test/dsp_tests.exe
