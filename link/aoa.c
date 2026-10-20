/*
 * aoa.c — CarLife AOA USB transport layer implementation (macOS / libusb)
 *
 * Handshake sequence taken from AOAHostSetup in the Android reference implementation
 * CarLife-Android-Vehicle:
 *   51 GET_PROTOCOL (read 2B, version = buf[1]<<8|buf[0])
 *   52 SEND_IDENT x6 (index 0..5)
 *   53 START_ACCESSORY (USB then re-enumerates, the old handle becomes invalid and the device must be re-enumerated)
 * For reference: request 58 (AUDIO_SUPPORT) is defined in the AOA protocol but CarLife never
 * calls it, so it is not implemented.
 */
#include "aoa.h"
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define ACCESSORY_VID 0x18D1
#define ACCESSORY_PID_MIN 0x2D00
#define ACCESSORY_PID_MAX 0x2D05
#define IPHONE_VID 0x05AC

#define REQ_TYPE_IN_VENDOR 0xC0
#define REQ_TYPE_OUT_VENDOR 0x40
#define AOA_GET_PROTOCOL 51
#define AOA_SEND_IDENT 52
#define AOA_START_ACCESSORY 53

#define EP0_TIMEOUT_MS 2000
#define BULK_TIMEOUT_MS 500
#define BULK_CHUNK 16384

#define RETRY_ACCESSORY_MODE 10

/* A zero-length bulk read is legal USB traffic (the phone emits zero-length packets
 * between transfers). Tolerate them, but stop waiting if they never end. */
#define MAX_ZERO_READS 64

static const char *const kIdent[6] = {
    "Baidu",
    "CarLife",
    "Baidu CarLife",
    "1.0.0",
    "http://carlife.baidu.com/",
    "0720SerialNo.",
};

static libusb_context *g_ctx = NULL;

/* Guards every stderr write in the process; see aoa_log_lock in aoa.h. */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void aoa_log_lock(void)
{
    pthread_mutex_lock(&g_log_lock);
}

void aoa_log_unlock(void)
{
    pthread_mutex_unlock(&g_log_lock);
}

/* Shared body of the aoa_log* functions. The level marker is written right after the
 * timestamp, which is where the Go backend looks for it when it relays these lines: it
 * becomes the line's level in the backend's own level column, and a line marked DEBUG is
 * dropped unless the backend runs at debug level. No marker means info. */
static void aoa_vlog(const char *level, const char *fmt, va_list ap)
{
    char ts[16];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    aoa_log_lock();
    fprintf(stderr, "[%s] ", ts);
    if (level != NULL) {
        fprintf(stderr, "%s ", level);
    }
    /* Subsystem token: the link process writes the USB layer's and the protocol stack's
     * output to one stream, so every line states which half it came from. The Go backend
     * reads it back into the module column (server/logging.go). */
    fputs("bridge ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    aoa_log_unlock();
}

void aoa_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    aoa_vlog(NULL, fmt, ap);
    va_end(ap);
}

/* Periodic diagnostics (the forwarding statistics) rather than events: they are worth
 * keeping for a session being watched live, and too repetitive to keep by default. */
void aoa_log_debug(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    aoa_vlog("DEBUG", fmt, ap);
    va_end(ap);
}

/* Something did not work but the link keeps running: a step of the device bring-up that
 * will be retried, a frame or a connection that had to be dropped. */
void aoa_log_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    aoa_vlog("WARN", fmt, ap);
    va_end(ap);
}

/* A failure: the operation was abandoned, or the link cannot continue and is about to be
 * rebuilt by the backend. */
void aoa_log_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    aoa_vlog("ERROR", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ utils */

static int is_accessory_pid(unsigned short vid, unsigned short pid)
{
    return vid == ACCESSORY_VID && pid >= ACCESSORY_PID_MIN && pid <= ACCESSORY_PID_MAX;
}

struct devnames {
    char maker[96];
    char prod[128];
};

/* Phone vendor keywords: on most phones iProduct is only the model name (e.g. "Pixel 8"),
 * so matching the vendor string is more reliable */
static const char *const kPhoneMaker[] = {
    "google", "xiaomi",  "redmi",  "poco",    "samsung",  "oppo",   "oneplus", "vivo",
    "huawei", "honor",   "meizu",  "realme",  "motorola", "sony",   "zte",     "nubia",
    "lenovo", "asus",    "nokia",  "infinix", "tecno",    "fairphone", "htc",  "sharp",
};

static void read_names(libusb_device_handle *h, const struct libusb_device_descriptor *d,
                       struct devnames *n)
{
    n->maker[0] = '\0';
    n->prod[0] = '\0';
    if (d->iManufacturer > 0) {
        libusb_get_string_descriptor_ascii(h, d->iManufacturer, (unsigned char *)n->maker,
                                          sizeof(n->maker));
    }
    if (d->iProduct > 0) {
        libusb_get_string_descriptor_ascii(h, d->iProduct, (unsigned char *)n->prod, sizeof(n->prod));
    }
}

static int looks_like_phone(const struct devnames *n)
{
    size_t i;
    if (strcasestr(n->prod, "android") != NULL || strcasestr(n->prod, "adb") != NULL) {
        return 1;
    }
    for (i = 0; i < sizeof(kPhoneMaker) / sizeof(kPhoneMaker[0]); i++) {
        if (strcasestr(n->maker, kPhoneMaker[i]) != NULL ||
            strcasestr(n->prod, kPhoneMaker[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

static const char *tag_for(const struct libusb_device_descriptor *d, const struct devnames *n)
{
    if (is_accessory_pid(d->idVendor, d->idProduct)) {
        return "already in accessory mode";
    }
    if (d->idVendor == IPHONE_VID) {
        return "skip: Apple device";
    }
    if (d->bDeviceClass == 0x03) {
        return "skip: HID";
    }
    if (d->bDeviceClass == 0x08) {
        return "skip: USB storage";
    }
    if (d->bDeviceClass == 0x09) {
        return "skip: Hub";
    }
    if (looks_like_phone(n)) {
        return "candidate: likely Android phone";
    }
    return "skip: not recognised as a phone";
}

/* Auto-pick score: 0 = not a candidate, 2 = likely phone, 3 = already accessory.
 * Anything that is neither an accessory nor recognisable as a phone scores 0 and is never
 * picked automatically -- hubs, keyboards, network adapters and the like would otherwise be
 * claimed while the phone is not plugged in yet, and the failed claim would abort startup.
 * A phone whose vendor string is not recognised can still be selected with --vid/--pid. */
static int score_for(const struct libusb_device_descriptor *d, const struct devnames *n)
{
    if (is_accessory_pid(d->idVendor, d->idProduct)) {
        return 3;
    }
    if (d->idVendor == IPHONE_VID) {
        return 0;
    }
    if (d->bDeviceClass == 0x03 || d->bDeviceClass == 0x08 || d->bDeviceClass == 0x09) {
        return 0;
    }
    return looks_like_phone(n) ? 2 : 0;
}

/* ------------------------------------------------------------------ init */

int aoa_init(void)
{
    int rc = libusb_init(&g_ctx);
    if (rc != 0) {
        aoa_log_error("libusb_init failed: %s", libusb_error_name(rc));
        g_ctx = NULL;
        return -1;
    }
    return 0;
}

void aoa_fini(void)
{
    if (g_ctx != NULL) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
}

void aoa_list_devices(void)
{
    libusb_device **list = NULL;
    ssize_t n, i;

    if (g_ctx == NULL) {
        return;
    }
    n = libusb_get_device_list(g_ctx, &list);
    if (n < 0) {
        aoa_log_error("failed to enumerate USB devices: %s", libusb_error_name((int)n));
        return;
    }
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        libusb_device_handle *h = NULL;
        struct devnames nm;

        nm.maker[0] = '\0';
        nm.prod[0] = '\0';
        if (libusb_get_device_descriptor(list[i], &d) != 0) {
            continue;
        }
        if (libusb_open(list[i], &h) == 0) {
            read_names(h, &d, &nm);
            libusb_close(h);
        } else {
            snprintf(nm.prod, sizeof(nm.prod), "(cannot open: held by another program?)");
        }
        aoa_log("  %04x:%04x  %-26s %-12s %s", d.idVendor, d.idProduct,
                nm.prod[0] ? nm.prod : "(no name)", nm.maker[0] ? nm.maker : "-", tag_for(&d, &nm));
    }
    libusb_free_device_list(list, 1);
}

/* ------------------------------------------------------------------ device pick */

static const char *error_hint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
        return "(device is busy: run adb kill-server first, or exit programs that grab the USB device)";
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return "(device is gone: the phone may have just re-enumerated)";
    }
    if (rc == LIBUSB_ERROR_IO) {
        return "(the endpoint failed: the phone has dropped the accessory connection)";
    }
    return "";
}

/* Scan the device list and pick and open the best one.
 * Returns 0 on success and fills *out (handle valid); returns -1 on failure. */
static int pick_and_open(int want_vid, int want_pid, struct aoa_device *out, int *is_accessory_out,
                         int verbose)
{
    libusb_device **list = NULL;
    ssize_t n, i;
    libusb_device_handle *best = NULL;
    libusb_device *best_dev = NULL;
    struct libusb_device_descriptor best_desc;
    int best_score = -1;
    int best_acc = 0;
    int want_specified = (want_vid >= 0 || want_pid >= 0);

    memset(&best_desc, 0, sizeof(best_desc));
    if (g_ctx == NULL) {
        return -1;
    }
    n = libusb_get_device_list(g_ctx, &list);
    if (n < 0) {
        aoa_log_error("failed to enumerate USB devices: %s", libusb_error_name((int)n));
        return -1;
    }

    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        libusb_device_handle *h = NULL;
        struct devnames nm;
        int acc, score;

        nm.maker[0] = '\0';
        nm.prod[0] = '\0';
        if (libusb_get_device_descriptor(list[i], &d) != 0) {
            continue;
        }
        if (libusb_open(list[i], &h) == 0) {
            read_names(h, &d, &nm);
        } else {
            h = NULL;
            snprintf(nm.prod, sizeof(nm.prod), "(cannot open)");
        }

        if (verbose) {
            aoa_log("  %04x:%04x  %-26s %-12s %s", d.idVendor, d.idProduct,
                    nm.prod[0] ? nm.prod : "(no name)", nm.maker[0] ? nm.maker : "-",
                    tag_for(&d, &nm));
        }

        acc = is_accessory_pid(d.idVendor, d.idProduct);
        if (want_specified) {
            score = -1;
            if ((want_vid < 0 || want_vid == (int)d.idVendor) &&
                (want_pid < 0 || want_pid == (int)d.idProduct)) {
                score = 3;
            }
        } else {
            score = score_for(&d, &nm);
        }

        if (score > 0 && score > best_score && h != NULL) {
            best_score = score;
            best_dev = list[i];
            best_desc = d;
            best_acc = acc;
            if (best != NULL) {
                libusb_close(best);
            }
            best = h;
            h = NULL;
        }
        if (h != NULL) {
            libusb_close(h);
        }
    }

    if (is_accessory_out != NULL) {
        *is_accessory_out = best_acc;
    }
    if (best != NULL && out != NULL) {
        memset(out, 0, sizeof(*out));
        out->handle = best;
        out->vid = best_desc.idVendor;
        out->pid = best_desc.idProduct;
        out->bus = libusb_get_bus_number(best_dev);
        out->address = libusb_get_device_address(best_dev);
    }
    libusb_free_device_list(list, 1);
    return best != NULL ? 0 : -1;
}

int aoa_open_target(int want_vid, int want_pid, struct aoa_device *out, int *is_accessory,
                    int verbose)
{
    return pick_and_open(want_vid, want_pid, out, is_accessory, verbose);
}

/* ------------------------------------------------------------------ handshake */

static int handshake_transfers(libusb_device_handle *h, int do_start)
{
    unsigned char ver[2] = {0, 0};
    unsigned int version;
    int i, r;

    r = libusb_control_transfer(h, REQ_TYPE_IN_VENDOR, AOA_GET_PROTOCOL, 0, 0, ver, 2,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        aoa_log_warn("  51 GET_PROTOCOL failed: %s %s", libusb_error_name(r), error_hint(r));
        return -1;
    }
    version = ((unsigned int)ver[1] << 8) | (unsigned int)ver[0];
    aoa_log("  51 GET_PROTOCOL  OK   device reports AOA version = %u", version);
    if (version < 1 || version > 2) {
        aoa_log_warn("  device does not support AOA 1.0/2.0, cannot continue");
        return -1;
    }

    for (i = 0; i < 6; i++) {
        unsigned char sbuf[64];
        size_t len = strlen(kIdent[i]);

        memcpy(sbuf, kIdent[i], len);
        r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_SEND_IDENT, 0, (unsigned short)i,
                                    sbuf, (unsigned short)len, EP0_TIMEOUT_MS);
        if (r < 0) {
            aoa_log_warn("  52 SEND_IDENT[index=%d] failed: %s %s", i, libusb_error_name(r), error_hint(r));
            return -1;
        }
    }
    aoa_log("  52 SEND_IDENT   OK   sent 6 identifiers");

    if (!do_start) {
        aoa_log("  skipped 53 START_ACCESSORY");
        return 0;
    }

    r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_START_ACCESSORY, 0, 0, NULL, 0,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        aoa_log_warn("  53 START_ACCESSORY failed: %s %s", libusb_error_name(r), error_hint(r));
        return -1;
    }
    aoa_log("  53 START_ACCESSORY OK   USB will re-enumerate, waiting...");
    return 0;
}

int aoa_handshake(struct aoa_device *d, int do_start)
{
    int rc = libusb_claim_interface(d->handle, 0);

    if (rc != 0) {
        aoa_log_warn("failed to claim interface 0: %s %s", libusb_error_name(rc), error_hint(rc));
        return -1;
    }
    rc = handshake_transfers(d->handle, do_start);
    libusb_release_interface(d->handle, 0);
    return rc;
}

int aoa_wait_accessory(int want_vid, int want_pid, struct aoa_device *out)
{
    int attempt;

    for (attempt = 1; attempt <= RETRY_ACCESSORY_MODE; attempt++) {
        struct aoa_device d;
        int acc = 0;

        if (aoa_stop) {
            return -1;
        }
        sleep(1);
        if (pick_and_open(want_vid, want_pid, &d, &acc, 0) == 0) {
            if (acc) {
                aoa_log("  attempt %d: entered accessory mode %04x:%04x", attempt, d.vid, d.pid);
                *out = d;
                return 0;
            }
            aoa_log("  attempt %d: found %04x:%04x, but not yet in accessory mode", attempt, d.vid, d.pid);
            aoa_close(&d);
        } else {
            aoa_log("  attempt %d: no openable device found", attempt);
        }
    }
    aoa_log_error("still not in accessory mode after %d retries", RETRY_ACCESSORY_MODE);
    return -1;
}

/* ------------------------------------------------------------------ endpoints */

/* accessory interface = class 0xff / subclass 0xff (the adb interface has subclass 0x42,
 * which is how they are told apart). The endpoint addresses vary with the configuration
 * (0x81/0x02 when accessory is alone, 0x81/0x01 when it coexists with adb), so they must be
 * read from the descriptors and cannot be hardcoded. The IN endpoint's max packet size is
 * read for the same reason: reads have to be aligned to it (see aoa_read_some). */
static int find_accessory_endpoints(libusb_device *dev, struct aoa_device *out)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, j, found = 0;

    out->ep_in = 0;
    out->ep_out = 0;
    out->ep_in_max_packet = 0;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        return -1;
    }
    for (i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        const struct libusb_interface_descriptor *alt;
        unsigned char in_addr = 0, out_addr = 0;
        unsigned short in_packet = 0;

        if (itf->num_altsetting <= 0) {
            continue;
        }
        alt = &itf->altsetting[0];
        if (alt->bInterfaceClass != 0xff || alt->bInterfaceSubClass != 0xff) {
            continue;
        }
        for (j = 0; j < alt->bNumEndpoints; j++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[j];

            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                continue;
            }
            if (ep->bEndpointAddress & 0x80) {
                in_addr = ep->bEndpointAddress;
                in_packet = ep->wMaxPacketSize;
            } else {
                out_addr = ep->bEndpointAddress;
            }
        }
        if (in_addr != 0 && out_addr != 0 && in_packet != 0) {
            out->ep_in = in_addr;
            out->ep_out = out_addr;
            out->ep_in_max_packet = in_packet;
            found = 1;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found ? 0 : -1;
}

int aoa_claim_accessory(struct aoa_device *d)
{
    int rc;

    if (find_accessory_endpoints(libusb_get_device(d->handle), d) != 0) {
        aoa_log_error("no bulk endpoints found on the accessory interface (class 0xff / subclass 0xff)");
        return -1;
    }
    rc = libusb_claim_interface(d->handle, 0);
    if (rc != 0) {
        aoa_log_warn("failed to claim interface 0: %s %s", libusb_error_name(rc), error_hint(rc));
        return -1;
    }
    d->claimed = 1;
    return 0;
}

void aoa_close(struct aoa_device *d)
{
    if (d->handle == NULL) {
        return;
    }
    if (d->claimed) {
        libusb_release_interface(d->handle, 0);
        d->claimed = 0;
    }
    libusb_close(d->handle);
    d->handle = NULL;
}

void aoa_reset_port(struct aoa_device *d)
{
    int r = libusb_reset_device(d->handle);

    if (r == 0) {
        aoa_log("  port reset accepted, the phone will re-enumerate");
    } else if (r == LIBUSB_ERROR_NOT_FOUND) {
        aoa_log("  the phone left the bus during the reset");
    } else {
        aoa_log_warn("  port reset failed: %s %s", libusb_error_name(r), error_hint(r));
    }
    aoa_close(d);
}

/* ------------------------------------------------------------------ send/receive */

static void put_be32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

int aoa_send_message(struct aoa_device *d, unsigned int channel, const unsigned char *msg,
                     unsigned int len)
{
    unsigned char aoa_head[AOA_HEAD_LEN];
    unsigned int done = 0;
    int sent = 0;
    int r;

    if (channel < 1 || channel > AOA_CHANNEL_COUNT) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (len == 0 || len > AOA_MSG_MAX) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    put_be32(aoa_head, channel);
    put_be32(aoa_head + 4, len);

    /* The AOA head and the message body must be sent as two separate bulk transfers --
     * the reference implementation also issues two consecutive bulkTransferOut calls */
    r = libusb_bulk_transfer(d->handle, d->ep_out, aoa_head, AOA_HEAD_LEN, &sent, BULK_TIMEOUT_MS);
    if (r != 0) {
        return r;
    }
    if (sent != AOA_HEAD_LEN) {
        return LIBUSB_ERROR_IO;
    }

    /* Message body in 16KB chunks (the AOA per-transfer bulk limit) */
    while (done < len) {
        unsigned int want = len - done;

        if (want > BULK_CHUNK) {
            want = BULK_CHUNK;
        }
        r = libusb_bulk_transfer(d->handle, d->ep_out, (unsigned char *)(msg + done), (int)want,
                                 &sent, BULK_TIMEOUT_MS);
        if (r != 0) {
            return r;
        }
        if (sent <= 0) {
            return LIBUSB_ERROR_IO;
        }
        done += (unsigned int)sent;
    }
    return 0;
}

/* Reads from the bulk IN endpoint. The request length is rounded down to a whole number of
 * max-size packets: a bulk read whose buffer is not a whole number of packets can be overrun
 * by the device, which libusb reports as LIBUSB_ERROR_OVERFLOW and macOS as kIOReturnOverrun
 * ("packet too large or more data than buffer"), and which leaves the endpoint unusable. The
 * device is free to send fewer bytes than requested, so callers must cope with any length.
 * See libusb's "Packets and overflows". */
int aoa_read_some(struct aoa_device *d, unsigned char *dst, unsigned int cap)
{
    unsigned int zero_reads = 0;

    if (cap > BULK_CHUNK) {
        cap = BULK_CHUNK;
    }
    cap -= cap % d->ep_in_max_packet;

    for (;;) {
        int transferred = 0;
        int r;

        if (aoa_stop) {
            return AOA_READ_STOPPED;
        }
        r = libusb_bulk_transfer(d->handle, d->ep_in, dst, (int)cap, &transferred,
                                 BULK_TIMEOUT_MS);
        if (r == LIBUSB_ERROR_TIMEOUT) {
            continue; /* periodic timeout, let aoa_stop decide whether to exit */
        }
        if (r != 0) {
            const char *hint = error_hint(r);
            aoa_log_error("USB read failed: %s%s%s; the link stops and the backend rebuilds it",
                          libusb_error_name(r), hint[0] != '\0' ? " " : "", hint);
            return -1;
        }
        if (transferred > 0) {
            return transferred;
        }
        /* The phone ends a transfer with a zero-length packet; that is not a link
         * failure, so keep waiting instead of tearing the whole bridge down. */
        if (++zero_reads >= MAX_ZERO_READS) {
            aoa_log_error("USB read returned 0 bytes %u times in a row, giving up", zero_reads);
            return -1;
        }
    }
}
