/*
 * aoa-probe — CarLife AOA path probe (macOS / libusb)
 *
 * Purpose: verify whether the head unit can switch an Android phone into AOA accessory
 *          mode, and confirm whether the CarLife USB frame format is
 *          [4B channelId][4B totalLen][CarLife msg] (big-endian).
 *
 * Usage:
 *   ./aoa-probe --list                    list USB devices only
 *   ./aoa-probe                           full probe: handshake -> re-enumerate -> read frames for 10 s
 *   ./aoa-probe --handshake-only          send 51/52 only, not 53 (phone USB state unchanged)
 *   ./aoa-probe --force-handshake         resend 51/52/53 even if the device is already in accessory mode
 *                                         (overwrite a stale accessory session on the phone with the CarLife descriptor)
 *   ./aoa-probe --send-proto-version      after entering accessory, send the first cmd frame
 *                                         MSG_CMD_HU_PROTOCOL_VERSION(0x00018001), then read the response
 *   ./aoa-probe --read 0                  do not read frames after a successful handshake
 *   ./aoa-probe --read 30                 read frames for 30 s
 *   ./aoa-probe --vid 0x2717 --pid 0x1234  select the target device (default: auto-pick)
 *
 * Run `adb kill-server` before this program: the adb server holds the USB device through
 * libusb, which makes libusb_claim_interface fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

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
#define AOA_LEN_MIN 8u
#define AOA_LEN_MAX (64u * 1024u * 1024u)
#define CHANNEL_COUNT 6
#define RETRY_ACCESSORY_MODE 10
#define RETRY_RESCAN 5
#define SEND_RETRY 20
#define DETAIL_FRAMES 20

static const char *const kIdent[6] = {
    "Baidu",
    "CarLife",
    "Baidu CarLife",
    "1.0.0",
    "http://carlife.baidu.com/",
    "0720SerialNo.",
};

static const char *const kChannelName[CHANNEL_COUNT + 1] = {
    "", "cmd", "video", "audio", "tts", "vr", "touch",
};

struct known_id {
    unsigned long id;
    const char *name;
};

/* EPackageHeadType from CarLife-Vehicle-Lib/LibSource/include/CTranRecvPackageProcess.h.
 * Naming rule: the highest bit of the low 16 bits (0x8000) encodes direction --
 * set = sent by the head unit (HU), clear = sent by the phone (MD). */
static const struct known_id kKnownId[] = {
    {0x00018001UL, "MSG_CMD_HU_PROTOCOL_VERSION"},
    {0x00010002UL, "MSG_CMD_PROTOCOL_VERSION_MATCH_STATUS"},
    {0x00018003UL, "MSG_CMD_HU_INFO"},
    {0x00010004UL, "MSG_CMD_MD_INFO"},
    {0x00018005UL, "MSG_CMD_HU_BT_PAIR_INFO"},
    {0x00010006UL, "MSG_CMD_MD_BT_PAIR_INFO"},
    {0x00018007UL, "MSG_CMD_VIDEO_ENCODER_INIT"},
    {0x00010008UL, "MSG_CMD_VIDEO_ENCODER_INIT_DONE"},
    {0x00018009UL, "MSG_CMD_VIDEO_ENCODER_START"},
    {0x0001800BUL, "MSG_CMD_VIDEO_ENCODER_RESET"},
    {0x0001800EUL, "MSG_CMD_PAUSE_MEDIA"},
    {0x00010018UL, "MSG_CMD_SCREEN_ON"},
    {0x00010019UL, "MSG_CMD_SCREEN_OFF"},
    {0x0001001BUL, "MSG_CMD_FOREGROUND"},
    {0x0001001CUL, "MSG_CMD_BACKGROUND"},
    {0x0001801DUL, "MSG_CMD_LAUNCH_MODE_NORMAL"},
    {0x00010026UL, "MSG_CMD_MODULE_STATUS"},
    {0x00018027UL, "MSG_CMD_STATISTIC_INFO"},
    {0x00018028UL, "MSG_CMD_MODULE_CONTROL"},
};

struct opts {
    int list_only;
    int handshake_only;
    int force_handshake;
    int send_proto;
    int read_seconds;
    int want_vid;
    int want_pid;
};

struct pick {
    unsigned short vid;
    unsigned short pid;
    int is_accessory;
    char product[128];
};

static unsigned int be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static const char *lookup_id(unsigned long id)
{
    size_t i;
    for (i = 0; i < sizeof(kKnownId) / sizeof(kKnownId[0]); i++) {
        if (kKnownId[i].id == id) {
            return kKnownId[i].name;
        }
    }
    return NULL;
}

static void print_hex(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        printf(" %02x", p[i]);
    }
}

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
    "google", "xiaomi", "redmi", "poco",   "samsung", "oppo",   "oneplus", "vivo",
    "huawei", "honor",  "meizu", "realme", "motorola", "sony",  "zte",     "nubia",
    "lenovo", "asus",   "nokia", "infinix", "tecno",  "fairphone", "htc",   "sharp",
};

static int is_apple(unsigned short vid)
{
    return vid == IPHONE_VID;
}

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

/* Device role label (used for list output and auto-pick) */
static const char *tag_for(const struct libusb_device_descriptor *d, const struct devnames *n)
{
    if (is_accessory_pid(d->idVendor, d->idProduct)) {
        return "already in accessory mode";
    }
    if (is_apple(d->idVendor)) {
        return d->idProduct == 0x12A8 ? "skip: iPhone" : "skip: other Apple device";
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
    return "candidate: other device (not a phone?)";
}

/* Auto-pick score: 0 = not a candidate, 1 = weak candidate, 2 = likely phone, 3 = already accessory */
static int score_for(const struct libusb_device_descriptor *d, const struct devnames *n)
{
    if (is_accessory_pid(d->idVendor, d->idProduct)) {
        return 3;
    }
    if (is_apple(d->idVendor)) {
        return 0;
    }
    if (d->bDeviceClass == 0x03 || d->bDeviceClass == 0x08 || d->bDeviceClass == 0x09) {
        return 0;
    }
    return looks_like_phone(n) ? 2 : 1;
}

static void list_devices(libusb_context *ctx)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    ssize_t i;
    if (n < 0) {
        printf("failed to enumerate USB devices: %s\n", libusb_error_name((int)n));
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
            strcpy(nm.prod, "(cannot open: held by another program?)");
        }
        printf("  %04x:%04x  %-26s %-12s %s\n", d.idVendor, d.idProduct,
               nm.prod[0] ? nm.prod : "(no name)", nm.maker[0] ? nm.maker : "-", tag_for(&d, &nm));
    }
    libusb_free_device_list(list, 1);
}

static const char *error_hint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
        return "(device is busy: run adb kill-server first, or unplug and close MTP/file-transfer programs)";
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return "(device is gone: the phone may have just re-enumerated, just run again)";
    }
    return "";
}

static void describe_config(libusb_device *dev)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, j;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        printf("  cannot read configuration descriptor\n");
        return;
    }
    for (i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        if (itf->num_altsetting <= 0) {
            continue;
        }
        const struct libusb_interface_descriptor *alt = &itf->altsetting[0];
        printf("  interface %u: class=0x%02x subclass=0x%02x, %u endpoint(s)\n",
               alt->bInterfaceNumber, alt->bInterfaceClass, alt->bInterfaceSubClass,
               alt->bNumEndpoints);
        for (j = 0; j < alt->bNumEndpoints; j++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[j];
            const char *dir = (ep->bEndpointAddress & 0x80) ? "IN " : "OUT";
            printf("    endpoint[%d] addr=0x%02x %s type=%s maxPacket=%u\n", j, ep->bEndpointAddress, dir,
                   (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK
                       ? "bulk"
                       : "non-bulk",
                   ep->wMaxPacketSize);
        }
    }
    libusb_free_config_descriptor(cfg);
}

/* Scan the device list: print all devices, pick the best one and open it.
 * Priority comes from score_for; once --vid/--pid is given, only an exact match counts.
 * Returns the handle (NULL on failure); the list is freed inside this function while the
 * returned handle stays valid (libusb_open holds its own device reference). */
static libusb_device_handle *open_target(libusb_context *ctx, const struct opts *o,
                                         struct pick *out, int *out_score, int verbose)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *best_handle = NULL;
    struct pick best;
    int best_score = -1;
    ssize_t i;
    int want_specified = (o->want_vid >= 0 || o->want_pid >= 0);

    memset(&best, 0, sizeof(best));
    if (n < 0) {
        printf("failed to enumerate USB devices: %s\n", libusb_error_name((int)n));
        return NULL;
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
            strcpy(nm.prod, "(cannot open)");
        }

        if (verbose) {
            printf("  %04x:%04x  %-26s %-12s %s\n", d.idVendor, d.idProduct,
                   nm.prod[0] ? nm.prod : "(no name)", nm.maker[0] ? nm.maker : "-",
                   tag_for(&d, &nm));
        }

        acc = is_accessory_pid(d.idVendor, d.idProduct);
        if (want_specified) {
            score = -1;
            if ((o->want_vid < 0 || o->want_vid == (int)d.idVendor) &&
                (o->want_pid < 0 || o->want_pid == (int)d.idProduct)) {
                score = 3;
            }
        } else {
            score = score_for(&d, &nm);
        }

        if (score > 0 && score > best_score && h != NULL) {
            best_score = score;
            best.vid = d.idVendor;
            best.pid = d.idProduct;
            best.is_accessory = acc;
            snprintf(best.product, sizeof(best.product), "%s", nm.prod);
            if (best_handle != NULL) {
                libusb_close(best_handle);
            }
            best_handle = h;
            h = NULL;
        }
        if (h != NULL) {
            libusb_close(h);
        }
    }

    libusb_free_device_list(list, 1);
    if (out_score != NULL) {
        *out_score = best_score;
    }
    if (best_handle != NULL) {
        *out = best;
    }
    return best_handle;
}

static int aoa_handshake(libusb_device_handle *h, int do_start)
{
    unsigned char ver[2] = {0, 0};
    unsigned int version;
    int i, r;

    r = libusb_control_transfer(h, REQ_TYPE_IN_VENDOR, AOA_GET_PROTOCOL, 0, 0, ver, 2, EP0_TIMEOUT_MS);
    if (r < 0) {
        printf("  51 GET_PROTOCOL failed: %s %s\n", libusb_error_name(r), error_hint(r));
        return -1;
    }
    version = ((unsigned int)ver[1] << 8) | (unsigned int)ver[0];
    printf("  51 GET_PROTOCOL  OK   device reports AOA version = %u\n", version);
    if (version < 1 || version > 2) {
        printf("  device does not support AOA 1.0/2.0, cannot continue\n");
        return -1;
    }

    for (i = 0; i < 6; i++) {
        unsigned char sbuf[64];
        size_t len = strlen(kIdent[i]);
        memcpy(sbuf, kIdent[i], len);
        r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_SEND_IDENT, 0, (unsigned short)i,
                                    sbuf, (unsigned short)len, EP0_TIMEOUT_MS);
        if (r < 0) {
            printf("  52 SEND_IDENT[index=%d] failed: %s %s\n", i, libusb_error_name(r),
                    error_hint(r));
            return -1;
        }
        printf("  52 SEND_IDENT   OK   index=%d = \"%s\"\n", i, kIdent[i]);
    }

    if (!do_start) {
        printf("  skipped 53 START_ACCESSORY (--handshake-only)\n");
        return 0;
    }

    r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_START_ACCESSORY, 0, 0, NULL, 0,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        printf("  53 START_ACCESSORY failed: %s %s\n", libusb_error_name(r), error_hint(r));
        return -1;
    }
    printf("  53 START_ACCESSORY OK  USB will re-enumerate, waiting...\n");
    return 0;
}

/* Wait for the phone to reappear in accessory mode */
static libusb_device_handle *wait_accessory(libusb_context *ctx, const struct opts *o,
                                            struct pick *out)
{
    int empty_rounds = 0;
    int attempt;
    for (attempt = 1; attempt <= RETRY_ACCESSORY_MODE; attempt++) {
        libusb_device_handle *h;
        sleep(1);
        h = open_target(ctx, o, out, NULL, 0);
        if (h != NULL) {
            if (out->is_accessory) {
                printf("  attempt %d: entered accessory mode %04x:%04x %s\n", attempt, out->vid,
                       out->pid, out->product);
                return h;
            }
            printf("  attempt %d: found device %04x:%04x, but not yet in accessory mode\n", attempt, out->vid,
                   out->pid);
            libusb_close(h);
            empty_rounds++;
        } else {
            printf("  attempt %d: no openable device found\n", attempt);
            empty_rounds++;
        }
        if (empty_rounds >= RETRY_RESCAN + RETRY_ACCESSORY_MODE) {
            break;
        }
    }
    printf("  still not in accessory mode after %d retries\n", RETRY_ACCESSORY_MODE);
    return NULL;
}

/* Wait for the given device to complete its USB re-enumeration.
 * When the device is already in accessory mode, VID:PID stays the same across the
 * re-enumeration, so it cannot distinguish the old and new nodes; the criterion for
 * completion is therefore "the device disappeared, or a device with the same VID:PID
 * changed its USB address". */
static void wait_rebind(libusb_context *ctx, unsigned short vid, unsigned short pid,
                        unsigned char old_addr, int max_seconds)
{
    int i;
    for (i = 0; i < max_seconds; i++) {
        libusb_device **list = NULL;
        ssize_t n = libusb_get_device_list(ctx, &list);
        ssize_t k;
        int same_node_still_there = 0;
        for (k = 0; k < n; k++) {
            struct libusb_device_descriptor d;
            if (libusb_get_device_descriptor(list[k], &d) != 0) {
                continue;
            }
            if (d.idVendor == vid && d.idProduct == pid &&
                libusb_get_device_address(list[k]) == old_addr) {
                same_node_still_there = 1;
                break;
            }
        }
        libusb_free_device_list(list, 1);
        if (!same_node_still_there) {
            printf("  original device node is gone (check %d)\n", i + 1);
            return;
        }
        sleep(1);
    }
    printf("  warning: no re-enumeration within %d s, continuing anyway\n", max_seconds);
}

static unsigned char *g_buf = NULL;
static size_t g_cap = 0;

static int ensure_buf(size_t n)
{
    unsigned char *p;
    if (n <= g_cap) {
        return 0;
    }
    p = (unsigned char *)realloc(g_buf, n);
    if (p == NULL) {
        return -1;
    }
    g_buf = p;
    g_cap = n;
    return 0;
}

/* Read exactly len bytes; returns 0 on success, 1 when the time window ends, -1 on error */
static int xfer_exact(libusb_device_handle *h, unsigned char ep, unsigned char *dst, unsigned int len,
                      time_t deadline)
{
    unsigned int done = 0;
    while (done < len) {
        unsigned int want = len - done;
        int transferred = 0;
        int r;
        if (time(NULL) >= deadline) {
            return 1;
        }
        if (want > BULK_CHUNK) {
            want = BULK_CHUNK;
        }
        r = libusb_bulk_transfer(h, ep, dst + done, (int)want, &transferred, BULK_TIMEOUT_MS);
        if (r == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (r != 0) {
            printf("  bulk read failed: %s %s\n", libusb_error_name(r), error_hint(r));
            return -1;
        }
        if (transferred <= 0) {
            printf("  bulk read returned 0 bytes\n");
            return -1;
        }
        done += (unsigned int)transferred;
    }
    return 0;
}

static void print_frame(unsigned long index, unsigned int ch, unsigned int len,
                        const unsigned char *p)
{
    const char *name = (ch >= 1 && ch <= CHANNEL_COUNT) ? kChannelName[ch] : "?";
    printf("[frame %lu] channel=%u(%s) totalLen=%u\n", index, ch, name, len);
    if (ch == 1) {
        unsigned long sid = be32(p + 4);
        const char *kn = lookup_id(sid);
        printf("        message head 8B:");
        print_hex(p, 8);
        printf("   serviceType=0x%08lx%s%s\n", sid, kn ? " → " : "", kn ? kn : "");
    } else if (ch == 6) {
        printf("        message head 8B:");
        print_hex(p, 8);
        printf("\n");
    } else {
        printf("        message head 12B:");
        print_hex(p, 12);
        printf("   body %u B\n", len > 12 ? len - 12 : 0);
    }
}

/* Find the bulk endpoints of the accessory interface in the current configuration.
 * accessory interface = class 0xff / subclass 0xff; the adb interface has subclass 0x42,
 * which is how they are told apart. The endpoint addresses vary with the configuration
 * (0x81/0x02 when accessory is alone, 0x81/0x01 when it coexists with adb), so they must be
 * read from the descriptors and cannot be hardcoded. Returns 0 on success. */
static int find_accessory_endpoints(libusb_device *dev, unsigned char *ep_in,
                                    unsigned char *ep_out)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, j, found = 0;

    *ep_in = 0;
    *ep_out = 0;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        return -1;
    }
    for (i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        const struct libusb_interface_descriptor *alt;
        unsigned char in = 0, out = 0;

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
                in = ep->bEndpointAddress;
            } else {
                out = ep->bEndpointAddress;
            }
        }
        if (in != 0 && out != 0) {
            *ep_in = in;
            *ep_out = out;
            found = 1;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found ? 0 : -1;
}

/* Send the first CarLife frame (cmd channel): MSG_CMD_HU_PROTOCOL_VERSION = 0x00018001.
 *
 * Frame layout (all big-endian):
 *   [4B channelId=1][4B totalLen]            AOA head
 *   [2B bodyLen][2B reserved][4B serviceType] CarLife cmd message head (8B)
 *   body                                      protobuf
 * totalLen = 8 + bodyLen (includes the inner message head, excludes the AOA head), matching
 * the reference implementation AOAConnectManager's `lenMsgHead + lenMsgData`.
 * The AOA head and the message body must be sent as two separate bulk transfers; the
 * reference implementation also issues two consecutive bulkTransferOut calls.
 *
 * body = CarlifeProtocolVersion{ majorVersion=1, minorVersion=0 }
 *      -> 08 01 (field1 varint=1) 10 00 (field2 varint=0), 4 bytes total.
 *
 * Returns 0 on success; a negative libusb error code on failure. Right after the
 * re-enumeration the phone-side app may not have obtained the accessory handle yet; in that
 * case OUT fails with LIBUSB_ERROR_TIMEOUT and the caller must retry. */
static int send_protocol_version(libusb_device_handle *h, unsigned char ep_out)
{
    unsigned char aoa[8];
    unsigned char msg[12];
    const unsigned int channel = 1;
    const unsigned int body_len = 4;
    const unsigned int total = 8 + body_len;
    const unsigned int service = 0x00018001u;
    int r, sent = 0;

    memset(msg, 0, sizeof(msg));
    msg[0] = (unsigned char)((body_len >> 8) & 0xff);
    msg[1] = (unsigned char)(body_len & 0xff);
    msg[4] = (unsigned char)((service >> 24) & 0xff);
    msg[5] = (unsigned char)((service >> 16) & 0xff);
    msg[6] = (unsigned char)((service >> 8) & 0xff);
    msg[7] = (unsigned char)(service & 0xff);
    msg[8] = 0x08;  /* protobuf field 1, varint */
    msg[9] = 0x01;  /* majorVersion = 1 */
    msg[10] = 0x10; /* protobuf field 2, varint */
    msg[11] = 0x00; /* minorVersion = 0 */

    aoa[0] = (unsigned char)((channel >> 24) & 0xff);
    aoa[1] = (unsigned char)((channel >> 16) & 0xff);
    aoa[2] = (unsigned char)((channel >> 8) & 0xff);
    aoa[3] = (unsigned char)(channel & 0xff);
    aoa[4] = (unsigned char)((total >> 24) & 0xff);
    aoa[5] = (unsigned char)((total >> 16) & 0xff);
    aoa[6] = (unsigned char)((total >> 8) & 0xff);
    aoa[7] = (unsigned char)(total & 0xff);

    r = libusb_bulk_transfer(h, ep_out, aoa, 8, &sent, BULK_TIMEOUT_MS);
    if (r != 0) {
        return r;
    }
    if (sent != 8) {
        return LIBUSB_ERROR_IO;
    }
    r = libusb_bulk_transfer(h, ep_out, msg, 12, &sent, BULK_TIMEOUT_MS);
    if (r != 0) {
        return r;
    }
    if (sent != 12) {
        return LIBUSB_ERROR_IO;
    }
    return 0;
}

static int read_frames(libusb_device_handle *h, unsigned char ep_in, unsigned char ep_out, int seconds)
{
    time_t deadline;
    unsigned long frames = 0;
    unsigned long per_ch_count[CHANNEL_COUNT + 1] = {0};
    unsigned long long per_ch_bytes[CHANNEL_COUNT + 1] = {0};
    unsigned long long total_bytes = 0;
    int printed = 0;
    time_t last_report;
    int rc = libusb_claim_interface(h, 0);
    int exit_code = 0;

    if (rc != 0) {
        printf("  failed to claim interface: %s %s\n", libusb_error_name(rc), error_hint(rc));
        return -1;
    }
    printf("  claimed interface 0\n");

    if (ep_out != 0) {
        int attempt, sr = 0;
        for (attempt = 1; attempt <= SEND_RETRY; attempt++) {
            sr = send_protocol_version(h, ep_out);
            if (sr == 0) {
                printf("  sent first cmd frame (attempt %d): AOA[channel=1 totalLen=12] + head[bodyLen=4 "
                       "reserved=0000 type=0x00018001] + body[08 01 10 00]\n",
                       attempt);
                break;
            }
            if (attempt == 1) {
                printf("  phone side not ready yet (%s), retrying...\n", libusb_error_name(sr));
            }
            sleep(1);
        }
        if (attempt > SEND_RETRY) {
            printf("  send failed: after %d retries still %s (the phone-side app may not have obtained accessory)\n", SEND_RETRY,
                   libusb_error_name(sr));
            libusb_release_interface(h, 0);
            return -1;
        }
    }

    deadline = time(NULL) + seconds;
    last_report = time(NULL);
    printf("  reading frames (%d s)...\n", seconds);

    for (;;) {
        unsigned char hdr[8];
        unsigned int ch, len;
        int st;
        time_t now;

        st = xfer_exact(h, ep_in, hdr, 8, deadline);
        if (st == 1) {
            break;
        }
        if (st < 0) {
            exit_code = -1;
            break;
        }

        ch = be32(hdr);
        len = be32(hdr + 4);
        if (ch < 1 || ch > CHANNEL_COUNT || len < AOA_LEN_MIN || len > AOA_LEN_MAX) {
            printf("  invalid frame head: channel=%u totalLen=%u (expected channel 1..6, len 8..64MB)\n", ch,
                    len);
            exit_code = -1;
            break;
        }
        if (ensure_buf(len) != 0) {
            printf("  failed to allocate %u-byte buffer\n", len);
            exit_code = -1;
            break;
        }
        st = xfer_exact(h, ep_in, g_buf, len, deadline);
        if (st == 1) {
            break;
        }
        if (st < 0) {
            exit_code = -1;
            break;
        }

        frames++;
        per_ch_count[ch]++;
        per_ch_bytes[ch] += len;
        total_bytes += len;
        if (printed < DETAIL_FRAMES) {
            print_frame(frames, ch, len, g_buf);
            printed++;
        }

        now = time(NULL);
        if (now != last_report) {
            last_report = now;
            printf("  ... accumulated %lu frames / %llu B (cmd=%lu video=%lu audio=%lu tts=%lu vr=%lu touch=%lu)\n",
                   frames, total_bytes, per_ch_count[1], per_ch_count[2], per_ch_count[3],
                   per_ch_count[4], per_ch_count[5], per_ch_count[6]);
        }
    }

    printf("  frame reading finished: %lu frames / %llu B total; per-channel frames cmd=%lu video=%lu audio=%lu tts=%lu vr=%lu touch=%lu\n",
           frames, total_bytes, per_ch_count[1], per_ch_count[2], per_ch_count[3], per_ch_count[4],
           per_ch_count[5], per_ch_count[6]);
    if (frames == 0) {
        printf("  no frames received: CarLife on the phone may not be running or the USB permission dialog was not accepted\n");
    }
    libusb_release_interface(h, 0);
    return exit_code;
}

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n", argv0);
    printf("  --list                 list USB devices only, do not touch any device\n");
    printf("  --handshake-only       send 51/52 only, not 53 (phone USB state unchanged)\n");
    printf("  --force-handshake      resend 51/52/53 even if already in accessory mode\n");
    printf("  --send-proto-version   send the first cmd frame MSG_CMD_HU_PROTOCOL_VERSION, then read the response\n");
    printf("  --read <seconds>       frame reading duration after entering accessory mode, default 10, 0 = do not read\n");
    printf("  --vid <0xXXXX>         target device VID\n");
    printf("  --pid <0xXXXX>         target device PID\n");
    printf("  -h, --help             show this help\n");
}

int main(int argc, char **argv)
{
    struct opts o;
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;
    struct pick pk;
    int i, rc;
    int score = -1;
    int exit_code = 0;

    memset(&o, 0, sizeof(o));
    o.read_seconds = 10;
    o.want_vid = -1;
    o.want_pid = -1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            o.list_only = 1;
        } else if (strcmp(argv[i], "--handshake-only") == 0) {
            o.handshake_only = 1;
        } else if (strcmp(argv[i], "--force-handshake") == 0) {
            o.force_handshake = 1;
        } else if (strcmp(argv[i], "--send-proto-version") == 0) {
            o.send_proto = 1;
        } else if (strcmp(argv[i], "--read") == 0 && i + 1 < argc) {
            o.read_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            o.want_vid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            o.want_pid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    rc = libusb_init(&ctx);
    if (rc != 0) {
        printf("libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    printf("== 1. enumerate USB devices ==\n");
    if (o.list_only) {
        list_devices(ctx);
        goto out;
    }
    h = open_target(ctx, &o, &pk, &score, 1);
    if (h == NULL) {
        printf("no usable target device found: make sure the phone is plugged in and no other program holds it\n"
               "  if the phone is plugged in, run adb kill-server first (the adb server holds the device via libusb)\n"
               "  you can also specify the device explicitly with --vid/--pid\n");
        exit_code = 1;
        goto out;
    }
    if (score < 2 && o.want_vid < 0 && o.want_pid < 0) {
        printf("no Android phone recognized; will not send AOA requests to this device without confirmation.\n"
               "  the auto-picked device is %04x:%04x %s\n"
               "  if this is correct, specify it explicitly: --vid 0x%04x --pid 0x%04x\n",
               pk.vid, pk.pid, pk.product, pk.vid, pk.pid);
        exit_code = 3;
        goto close_handle;
    }
    printf("selected: %04x:%04x  %s%s\n", pk.vid, pk.pid, pk.product,
           pk.is_accessory ? "  [already in accessory mode]" : "");

    printf("== 2. device descriptors ==\n");
    describe_config(libusb_get_device(h));

    if (pk.is_accessory && !o.force_handshake) {
        printf("== 3. handshake ==\n  device is already in accessory mode, skipping 51/52/53\n");
    } else {
        unsigned char old_addr = 0;
        int forced = pk.is_accessory;

        printf("== 3. AOA handshake ==\n");
        if (forced) {
            old_addr = libusb_get_device_address(libusb_get_device(h));
            printf("  --force-handshake: device is already in accessory mode; resending 51/52/53 to overwrite its descriptor\n");
        }
        rc = libusb_claim_interface(h, 0);
        if (rc != 0) {
            printf("  failed to claim interface 0: %s %s\n", libusb_error_name(rc), error_hint(rc));
            exit_code = 1;
            goto close_handle;
        }
        rc = aoa_handshake(h, !o.handshake_only);
        libusb_release_interface(h, 0);
        libusb_close(h);
        h = NULL;
        if (rc != 0) {
            exit_code = 1;
            goto out;
        }
        if (o.handshake_only) {
            printf("  done (--handshake-only, phone state unchanged)\n");
            goto out;
        }

        if (forced) {
            wait_rebind(ctx, pk.vid, pk.pid, old_addr, RETRY_ACCESSORY_MODE);
        }

        printf("== 4. waiting for accessory mode re-enumeration ==\n");
        h = wait_accessory(ctx, &o, &pk);
        if (h == NULL) {
            printf("result: AOA handshake was sent, but the device did not reappear in accessory mode\n");
            exit_code = 1;
            goto out;
        }
        printf("== 5. accessory device descriptors ==\n");
        describe_config(libusb_get_device(h));
    }

    if (o.read_seconds > 0 || o.send_proto) {
        unsigned char ep_in = 0x81;
        unsigned char ep_out = 0;
        printf("== 6. frame read verification ==\n");
        if (o.send_proto) {
            if (find_accessory_endpoints(libusb_get_device(h), &ep_in, &ep_out) != 0) {
                printf("  accessory interface not found (class 0xff / subclass 0xff), falling back to default endpoints\n");
                ep_in = 0x81;
                ep_out = 0x01;
            }
            printf("  accessory endpoints: IN=0x%02x OUT=0x%02x\n", ep_in, ep_out);
        }
        if (read_frames(h, ep_in, ep_out, o.read_seconds) != 0) {
            exit_code = 1;
        }
    } else {
        printf("== 6. frame read verification ==\n  skipped (--read 0)\n");
    }

close_handle:
    if (h != NULL) {
        libusb_close(h);
    }
out:
    free(g_buf);
    libusb_exit(ctx);
    printf("exit code %d\n", exit_code);
    return exit_code;
}
