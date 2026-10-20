# carlife-link -- the phone link: USB(AOA) transport + CarLife head-unit protocol stack
#
# Links against libcarlifevehicle.dylib, which `make sdk` in the repository root produces as
# part of the full build. The library is a prerequisite below, so it is never rebuilt from
# here and never forces a relink on its own.
#
# The transport half (aoa.c, bridge.c) is C and needs libusb; md_authen.c is C and needs
# libcrypto for the RSA check of the phone's authentication response; the protocol stack
# half (main.cpp) is C++ and needs the CarLife headers; gps.mm is Objective-C++ and needs
# CoreLocation for the Wi-Fi position. Only gps.mm is compiled as Objective-C++, but the
# final link is done with the C++ compiler because the library is C++.

ROOT       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PB         := $(ROOT)/.build/pb
CARLIFE    := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource

# Run-time artifacts go to dist/, where carlifed looks for its siblings. The library is
# resolved relative to this binary (rpath) instead of an absolute build path, so dist/ can
# be copied or moved as a whole.
#
# The binary lives inside an .app bundle because CoreLocation refuses a bare executable:
# without the usage-description keys in an Info.plist the authorization request fails
# immediately and the status stays notDetermined, so no fix ever arrives. The bundle is
# signed ad-hoc for the same reason -- an unsigned one is not granted either. Launching
# Contents/MacOS/carlife-link directly is enough; it does not have to be started with
# `open`, so the parent keeps the child's stdout and stderr.
DIST       := $(ROOT)/dist
BUNDLE     := $(DIST)/CarLifeLink.app

CC         ?= cc
CXX        ?= clang++
CFLAGS     += -O2 -g -Wall -Wextra -std=gnu11 $(shell pkg-config --cflags libusb-1.0 libcrypto)
CXXFLAGS   += -std=gnu++11 -O2 -Wall -Wextra -I$(CARLIFE)/include -I$(PB)/include
LDFLAGS    += -L$(DIST) -lcarlifevehicle -lpthread -Wl,-rpath,@loader_path/../../..
LDLIBS     += $(shell pkg-config --libs libusb-1.0 libcrypto) -framework CoreLocation -framework Foundation

TARGET     := $(BUNDLE)/Contents/MacOS/carlife-link
PLIST      := $(BUNDLE)/Contents/Info.plist
OBJECTS    := aoa.o bridge.o md_authen.o main.o gps.o
LIBRARY    := $(DIST)/libcarlifevehicle.dylib

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS) $(LIBRARY) Info.plist
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)
	@cp Info.plist $(PLIST)
	@codesign --force --sign - $(BUNDLE)
	@echo "Build complete: $(BUNDLE)"

aoa.o: aoa.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ $<

bridge.o: bridge.c aoa.h bridge.h
	$(CC) $(CFLAGS) -c -o $@ $<

md_authen.o: md_authen.c md_authen.h
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.cpp protocol.h bridge.h md_authen.h gps.h log.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

gps.o: gps.mm gps.h
	$(CXX) $(CXXFLAGS) -fobjc-arc -c -o $@ $<

# The library is a prerequisite rather than a phony existence check: this way the target is
# relinked when the library changes and not on every run. It is built by `make sdk` in the
# repository root -- building it from here would mean running the whole build from inside
# this one.
$(LIBRARY):
	@echo "$(LIBRARY) is missing, run make sdk in the repository root first" >&2
	@exit 1

clean:
	rm -f $(OBJECTS)
	rm -rf $(BUNDLE)
