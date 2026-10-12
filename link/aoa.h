/*
 * aoa.h — CarLife head-unit AOA (Android Open Accessory) USB transport layer
 *
 * Switch an Android phone into AOA accessory mode and send/receive CarLife frames
 * over a single USB channel.
 *
 * USB frame layout (all big-endian):
 *   [4B channelId][4B totalLen][CarLife message]
 * channelId 1..6 = cmd/video/audio/tts/vr/touch;
 * totalLen = CarLife message head length + message body length (excludes this 8B head).
 *
 * This module handles USB transport only; it does not parse CarLife message semantics.
 */
#ifndef CARLIFE_AOA_H
#define CARLIFE_AOA_H

#include <signal.h>

#include <libusb.h>

/* Process-wide stop flag. Defined by the host (bridge.c) and set in its signal
 * handler; this module only reads it. */
extern volatile sig_atomic_t aoa_stop;

#define AOA_CHANNEL_COUNT 6
#define AOA_HEAD_LEN 8

/* Maximum length of one CarLife message (video channel cap 600KB + 12B head) */
#define AOA_MSG_MAX (640u * 1024u)

struct aoa_device {
    libusb_device_handle *handle;
    unsigned char ep_in;
    unsigned char ep_out;
    int claimed;
    unsigned short vid;
    unsigned short pid;
    unsigned char bus;
    unsigned char address;
};

/* Timestamped log, written to stderr */
void aoa_log(const char *fmt, ...);

/* Same, marked debug: the Go backend relays this process's output and drops marked lines
 * unless it runs at debug level. Use it for periodic statistics, not for events. */
void aoa_log_debug(const char *fmt, ...);

int aoa_init(void);
void aoa_fini(void);

/* Enumerate and print all USB devices (does not touch any device) */
void aoa_list_devices(void);

/* Pick and open the target device without changing its USB state.
 * want_vid / want_pid = -1 means auto-pick.
 * Returns 0 on success; *is_accessory reports whether the device is already in accessory mode. */
int aoa_open_target(int want_vid, int want_pid, struct aoa_device *out, int *is_accessory,
                    int verbose);

/* Send AOA identify requests 51/52 (+ 53). With do_start=0, 53 is not sent and the
 * phone USB state is unchanged. Internally claims/releases interface 0. Returns 0 on success. */
int aoa_handshake(struct aoa_device *d, int do_start);

/* Wait for the device to re-enumerate in accessory mode and open it. Returns 0 on success. */
int aoa_wait_accessory(int want_vid, int want_pid, struct aoa_device *out);

/* Find the bulk endpoints on the accessory interface (class 0xff / subclass 0xff) and claim it. */
int aoa_claim_accessory(struct aoa_device *d);

/* Release the interface and close the device */
void aoa_close(struct aoa_device *d);

/* Send one full CarLife message: prepend the 8B AOA head, call bulk transfer in 16KB chunks.
 * The caller is responsible for serialization (never call concurrently on the same device).
 * Returns 0 on success, otherwise a negative libusb error code. */
int aoa_send_message(struct aoa_device *d, unsigned int channel, const unsigned char *msg,
                     unsigned int len);

/* Read exactly len bytes. Returns 0 on success, 1 if stop was requested, -1 on error. */
int aoa_read_exact(struct aoa_device *d, unsigned char *dst, unsigned int len);

#endif /* CARLIFE_AOA_H */
