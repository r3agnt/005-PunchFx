# Mac-side tests for the looper engine (plain C++, no Daisy hardware).
# Run from the project folder:  make -C test
# Built with address/undefined-behaviour checking so out-of-range buffer
# reads are caught too.
CXX      ?= c++
CXXFLAGS ?= -std=c++14 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined
HEADERS   = $(wildcard ../src/engine/*.h)

all: build/test_clock build/test_quantized_loop_buffer build/test_beat build/test_pitch build/test_fx build/test_saved_state
	./build/test_clock
	./build/test_quantized_loop_buffer
	./build/test_beat
	./build/test_pitch
	./build/test_fx
	./build/test_saved_state

build/test_clock: test_clock.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_clock.cpp -o build/test_clock

build/test_quantized_loop_buffer: test_quantized_loop_buffer.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_quantized_loop_buffer.cpp -o build/test_quantized_loop_buffer

build/test_beat: test_beat.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_beat.cpp -o build/test_beat

build/test_slice: test_slice.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_slice.cpp -o

clean:
	rm -rf build

build/test_pitch: test_pitch.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_pitch.cpp -o build/test_pitch

build/test_fx: test_fx.cpp $(HEADERS) $(wildcard ../src/engine/fx/*.h)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_fx.cpp -o build/test_fx

build/test_saved_state: test_saved_state.cpp $(HEADERS) $(wildcard ../src/engine/fx/*.h)
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_saved_state.cpp -o build/test_saved_state
