CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

aoa-probe: aoa-probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f aoa-probe

.PHONY: clean
