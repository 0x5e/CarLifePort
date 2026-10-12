# carlife-link -- the phone link: USB(AOA) transport + CarLife head-unit protocol stack
#
# Links against libcarlifevehicle.dylib, which scripts/build.sh produces (and rebuilds) as
# part of the full build. The check below only reports when it is missing; building it from
# here would mean running the whole build from inside itself, and the library is built from
# the vendored sources, so it never changes while this link does not.
#
# The transport half (aoa.c, bridge.c) is C and needs libusb; the protocol stack half
# (main.cpp) is C++ and needs the CarLife headers. Only main.cpp is compiled as C++, but the
# final link is done with the C++ compiler because the library is C++.

ROOT       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PB         := $(ROOT)/.build/pb
CARLIFE    := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource

# Run-time artifacts go to dist/, where carlifed looks for its siblings. The library is
# resolved relative to this binary (rpath @loader_path) instead of an absolute build path,
# so dist/ can be copied or moved as a whole.
DIST       := $(ROOT)/dist

CC         ?= cc
CXX        ?= clang++
CFLAGS     += -O2 -g -Wall -Wextra -std=gnu11 $(shell pkg-config --cflags libusb-1.0)
CXXFLAGS   += -std=gnu++11 -O2 -Wall -Wextra -I$(CARLIFE)/include -I$(PB)/include
LDFLAGS    += -L$(DIST) -lcarlifevehicle -lpthread -Wl,-rpath,@loader_path
LDLIBS     += $(shell pkg-config --libs libusb-1.0)

TARGET     := $(DIST)/carlife-link
OBJECTS    := aoa.o bridge.o main.o

.PHONY: all clean native

all: $(TARGET)

$(TARGET): $(OBJECTS) native
	@mkdir -p $(DIST)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)
	@echo "Build complete: $@"

aoa.o: aoa.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ $<

bridge.o: bridge.c aoa.h bridge.h
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.cpp protocol.h bridge.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

native $(DIST)/libcarlifevehicle.dylib:
	@test -f $(DIST)/libcarlifevehicle.dylib || \
	  { echo "$(DIST)/libcarlifevehicle.dylib is missing, run ./scripts/build.sh first" >&2; exit 1; }

clean:
	rm -f $(OBJECTS) $(TARGET)
