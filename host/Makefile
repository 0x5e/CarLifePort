# carlife-host -- host process for the CarLife head-unit-side protocol stack
#
# Depends on libcarlifevehicle.dylib produced by scripts/build.sh --native,
# and triggers a build automatically if it is not present yet.

ROOT       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PB         := $(ROOT)/.build/pb
CARLIFE    := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource

# Run-time artifacts go to dist/, where carlifed looks for its siblings. The library is
# resolved relative to this binary (rpath @loader_path) instead of an absolute build path,
# so dist/ can be copied or moved as a whole.
DIST       := $(ROOT)/dist

CXX        ?= clang++
CXXFLAGS   += -std=gnu++11 -O2 -Wall -Wextra -I$(CARLIFE)/include -I$(PB)/include
LDFLAGS    += -L$(DIST) -lcarlifevehicle -lpthread -Wl,-rpath,@loader_path

TARGET     := $(DIST)/carlife-host
SOURCES    := main.cpp
HEADERS    := protocol.h

.PHONY: all clean native

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) native
	@mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)
	@echo "Build complete: $@"

native $(DIST)/libcarlifevehicle.dylib:
	@test -f $(DIST)/libcarlifevehicle.dylib || $(ROOT)/scripts/build.sh --native

clean:
	rm -f $(TARGET)
