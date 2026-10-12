CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0) -lpthread

# Run-time artifacts go to dist/ so that carlifed finds this binary next to itself.
# The .o files stay here.
DIST = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../dist)
TARGET = $(DIST)/carlife-usb-bridge
OBJS = aoa.o bridge.o

$(TARGET): $(OBJS)
	@mkdir -p $(DIST)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

aoa.o: aoa.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ aoa.c

bridge.o: bridge.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ bridge.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
