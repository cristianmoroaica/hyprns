# hyprns: live Navier–Stokes wallpapers drawn by Hyprland itself.
# Builds against the Hyprland headers that are installed (pkg-config hyprland);
# the plugin must be rebuilt after every Hyprland update.

CXX ?= g++
CXXFLAGS = -fPIC -g -O2 -std=c++23 -Wall -Wextra -Wno-unused-parameter
# glibc 2.41+ refuses to load a library that asks for an executable stack.
LDFLAGS = -shared -Wl,-z,noexecstack
INCLUDES = -Isrc $(shell pkg-config --cflags hyprland pixman-1 libdrm)
LIBS = $(shell pkg-config --libs hyprland)

# Keeps two loaded copies (unload, rebuild, load) from sharing globals. GCC only.
ifneq ($(shell $(CXX) --version 2>/dev/null | grep -c 'Free Software Foundation'),0)
	CXXFLAGS += --no-gnu-unique
endif

TARGET = hyprns.so
SOURCES = $(shell find src -name '*.cpp')
OBJ = $(patsubst src/%.cpp,build/%.o,$(SOURCES))
HEADERS = $(shell find src -name '*.hpp')

all: $(TARGET)

build/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[$(CXX)] $<"
	@$(CXX) -c $(CXXFLAGS) $(INCLUDES) $< -o $@

# Linked under another name and renamed: a compositor that has the old file loaded
# keeps its own inode instead of seeing its code rewritten underneath it.
$(TARGET): $(OBJ)
	@$(CXX) $(LDFLAGS) $(OBJ) -o $@.tmp $(LIBS)
	@mv -f $@.tmp $@
	@echo "built $(TARGET)"

# hyprns-bench: the looks and Gl in a headless EGL context, no Hyprland. Built from
# its own objects without the Hyprland include path, so a look that includes a
# Hyprland header does not build here.
BENCH = build/hyprns-bench
BENCH_CXXFLAGS = -g -O2 -std=c++23 -Wall -Wextra -Wno-unused-parameter
BENCH_INCLUDES = -Isrc $(shell pkg-config --cflags egl glesv2 libpng)
BENCH_LIBS = $(shell pkg-config --libs egl glesv2 libpng)
# src/core, src/gl, src/looks: everything without Hyprland (the bench and the tests)
PURE_SOURCES = $(shell find src/core src/gl src/looks -name '*.cpp')
PURE_OBJ = $(patsubst %.cpp,build/bench/%.o,$(PURE_SOURCES))
BENCH_SOURCES = bench/main.cpp $(PURE_SOURCES)
BENCH_OBJ = $(patsubst %.cpp,build/bench/%.o,$(BENCH_SOURCES))

bench: $(BENCH)

build/bench/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[$(CXX) bench] $<"
	@$(CXX) -c $(BENCH_CXXFLAGS) $(BENCH_INCLUDES) $< -o $@

$(BENCH): $(BENCH_OBJ)
	@$(CXX) $(BENCH_OBJ) -o $@ $(BENCH_LIBS)
	@echo "built $(BENCH)"

# Host unit tests: no GL context is made (the GL libraries are only linked, for the
# looks the registry refers to). check.hpp is also compiled once on its own as the main.
UNIT = build/unit-tests
UNIT_SOURCES = $(wildcard test/unit/*.cpp)
UNIT_OBJ = $(patsubst %.cpp,build/%.o,$(UNIT_SOURCES)) build/test/unit/main.o $(PURE_OBJ)

build/test/unit/%.o: test/unit/%.cpp test/unit/check.hpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[$(CXX) test] $<"
	@$(CXX) -c $(BENCH_CXXFLAGS) $(BENCH_INCLUDES) -Itest/unit $< -o $@

build/test/unit/main.o: test/unit/check.hpp
	@mkdir -p $(dir $@)
	@$(CXX) -c $(BENCH_CXXFLAGS) -DHYPRNS_CHECK_MAIN -include $< -x c++ /dev/null -o $@

$(UNIT): $(UNIT_OBJ)
	@$(CXX) $(UNIT_OBJ) -o $@ $(BENCH_LIBS)

test: $(UNIT)
	@./$(UNIT)

clean:
	rm -rf build $(TARGET)

.PHONY: all bench test clean
