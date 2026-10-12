# carlife-link -- the phone link: USB(AOA) transport + CarLife head-unit protocol stack
#
# Depends on libcarlifevehicle.dylib produced by scripts/build.sh --native, and triggers a
# build automatically if it is not present yet.
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
	@test -f $(DIST)/libcarlifevehicle.dylib || $(ROOT)/scripts/build.sh --native

clean:
	rm -f $(OBJECTS) $(TARGET)
