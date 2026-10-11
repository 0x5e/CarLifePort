# carlife-host -- host process for the CarLife head-unit-side protocol stack
#
# Depends on libcarlifevehicle.dylib produced by scripts/build-native.sh,
# and triggers a build automatically if it is not present yet.

ROOT       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PB         := $(ROOT)/.build/pb
LIBDIR     := $(ROOT)/.build/lib
CARLIFE    := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource

CXX        ?= clang++
CXXFLAGS   += -std=gnu++11 -O2 -Wall -Wextra -I$(CARLIFE)/include -I$(PB)/include
LDFLAGS    += -L$(LIBDIR) -lcarlifevehicle -lpthread -Wl,-rpath,$(LIBDIR)

TARGET     := carlife-host
SOURCES    := main.cpp
HEADERS    := protocol.h

.PHONY: all clean native

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) native
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)
	@echo "Build complete: $@"

native $(LIBDIR)/libcarlifevehicle.dylib:
	@test -f $(LIBDIR)/libcarlifevehicle.dylib || $(ROOT)/scripts/build-native.sh

clean:
	rm -f $(TARGET)
