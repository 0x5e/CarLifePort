/*
 * bridge.h — the USB half of carlife-link.
 *
 * Owns the phone-facing side: the AOA handshake, the USB bulk transfer loop, and one local
 * TCP port per CarLife channel that CarLifeVehicleLib connects to (7200 cmd, 8200 video,
 * 9200 audio, 9201 tts, 9202 vr, 9300 touch).
 *
 * main.cpp drives this API: bridge_start() claims a phone and opens the six ports, then
 * bridge_loop() forwards frames until the link drops. The order matters and is the same one
 * two separate processes used to be started in: the transport has to be listening before
 * the library connects.
 */
#ifndef CARLIFE_LINK_BRIDGE_H
#define CARLIFE_LINK_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enumerate the USB devices and print them; does not touch any device. Returns 0 or 1. */
int bridge_list_devices(void);

/* Claim a phone (vid/pid = -1 to auto-pick) and open the six local channel ports. Blocks
 * until a device is acquired, because the phone may be plugged in after the process
 * started. Also installs the process's SIGINT/SIGTERM handler (sets the transport stop
 * flag) and ignores SIGPIPE. Returns 0 on success, 1 if the device or the ports could not
 * be obtained. */
int bridge_start(const char *host, int vid, int pid, int quiet);

/* Forward frames between USB and the six local channels until the link drops or a stop was
 * requested. Returns 0 for a clean stop and -1 when the link failed, which means the
 * process should exit so that carlifed restarts it. */
int bridge_loop(void);

/* Stop the transport and release the device. Called once on the way out. */
void bridge_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CARLIFE_LINK_BRIDGE_H */
