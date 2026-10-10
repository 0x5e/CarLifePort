# carlife-host —— CarLife 车机侧协议栈宿主进程
#
# 依赖 scripts/build-native.sh 产出的 libcarlifevehicle.dylib，
# 若尚未构建会自动触发一次。

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
	@echo "构建完成：$@"

native $(LIBDIR)/libcarlifevehicle.dylib:
	@test -f $(LIBDIR)/libcarlifevehicle.dylib || $(ROOT)/scripts/build-native.sh

clean:
	rm -f $(TARGET)
