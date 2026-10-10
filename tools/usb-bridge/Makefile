CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0) -lpthread

TARGET = carlife-usb-bridge
OBJS = aoa.o bridge.o

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

aoa.o: aoa.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ aoa.c

bridge.o: bridge.c aoa.h
	$(CC) $(CFLAGS) -c -o $@ bridge.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
