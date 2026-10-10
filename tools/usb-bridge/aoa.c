/*
 * aoa.c — CarLife AOA USB 传输层实现（macOS / libusb）
 *
 * 握手序列取自 Android 参考实现 CarLife-Android-Vehicle 的 AOAHostSetup：
 *   51 GET_PROTOCOL（读 2B，version = buf[1]<<8|buf[0]）
 *   52 SEND_IDENT ×6（index 0..5）
 *   53 START_ACCESSORY（此后 USB 逻辑插拔，原句柄失效，必须重新枚举）
 * 供参考：请求 58（AUDIO_SUPPORT）虽在 AOA 协议中定义，CarLife 从未调用，故不实现。
 */
#include "aoa.h"

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
#define RETRY_REBIND 10

static const char *const kIdent[6] = {
    "Baidu",
    "CarLife",
    "Baidu CarLife",
    "1.0.0",
    "http://carlife.baidu.com/",
    "0720SerialNo.",
};

static libusb_context *g_ctx = NULL;

void aoa_log(const char *fmt, ...)
{
    va_list ap;
    char ts[16];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ 工具 */

static int is_accessory_pid(unsigned short vid, unsigned short pid)
{
    return vid == ACCESSORY_VID && pid >= ACCESSORY_PID_MIN && pid <= ACCESSORY_PID_MAX;
}

struct devnames {
    char maker[96];
    char prod[128];
};

/* 手机厂商关键字：多数手机的 iProduct 只是机型名（如 "Pixel 8"），靠厂商串识别更可靠 */
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
        return "已进入 accessory 模式";
    }
    if (d->idVendor == IPHONE_VID) {
        return "跳过：Apple 设备";
    }
    if (d->bDeviceClass == 0x03) {
        return "跳过：HID";
    }
    if (d->bDeviceClass == 0x08) {
        return "跳过：USB 存储";
    }
    if (d->bDeviceClass == 0x09) {
        return "跳过：Hub";
    }
    if (looks_like_phone(n)) {
        return "候选：疑似 Android 手机";
    }
    return "候选：其它设备（非手机？）";
}

/* 自动挑选打分：0 = 不候选，1 = 弱候选，2 = 疑似手机，3 = 已在 accessory */
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
    return looks_like_phone(n) ? 2 : 1;
}

/* ------------------------------------------------------------------ 初始化 */

int aoa_init(void)
{
    int rc = libusb_init(&g_ctx);
    if (rc != 0) {
        aoa_log("libusb_init 失败：%s", libusb_error_name(rc));
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
        aoa_log("枚举 USB 设备失败：%s", libusb_error_name((int)n));
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
            snprintf(nm.prod, sizeof(nm.prod), "(无法打开：被其它程序占用？)");
        }
        aoa_log("  %04x:%04x  %-26s %-12s %s", d.idVendor, d.idProduct,
                nm.prod[0] ? nm.prod : "(无名称)", nm.maker[0] ? nm.maker : "-", tag_for(&d, &nm));
    }
    libusb_free_device_list(list, 1);
}

/* ------------------------------------------------------------------ 设备挑选 */

static const char *error_hint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
        return "（设备被占用：先执行 adb kill-server，或退出会抢占 USB 的程序）";
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return "（设备已消失：手机可能刚重新枚举）";
    }
    return "";
}

/* 扫一遍设备列表，挑选并打开最合适的一个。
 * 成功返回 0 并填充 *out（handle 有效）；失败返回 -1。 */
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
        aoa_log("枚举 USB 设备失败：%s", libusb_error_name((int)n));
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
            snprintf(nm.prod, sizeof(nm.prod), "(无法打开)");
        }

        if (verbose) {
            aoa_log("  %04x:%04x  %-26s %-12s %s", d.idVendor, d.idProduct,
                    nm.prod[0] ? nm.prod : "(无名称)", nm.maker[0] ? nm.maker : "-",
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

/* ------------------------------------------------------------------ 握手 */

static int handshake_transfers(libusb_device_handle *h, int do_start)
{
    unsigned char ver[2] = {0, 0};
    unsigned int version;
    int i, r;

    r = libusb_control_transfer(h, REQ_TYPE_IN_VENDOR, AOA_GET_PROTOCOL, 0, 0, ver, 2,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        aoa_log("  51 GET_PROTOCOL 失败：%s %s", libusb_error_name(r), error_hint(r));
        return -1;
    }
    version = ((unsigned int)ver[1] << 8) | (unsigned int)ver[0];
    aoa_log("  51 GET_PROTOCOL  OK   设备报告 AOA 版本 = %u", version);
    if (version < 1 || version > 2) {
        aoa_log("  该设备不支持 AOA 1.0/2.0，无法继续");
        return -1;
    }

    for (i = 0; i < 6; i++) {
        unsigned char sbuf[64];
        size_t len = strlen(kIdent[i]);

        memcpy(sbuf, kIdent[i], len);
        r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_SEND_IDENT, 0, (unsigned short)i,
                                    sbuf, (unsigned short)len, EP0_TIMEOUT_MS);
        if (r < 0) {
            aoa_log("  52 SEND_IDENT[index=%d] 失败：%s %s", i, libusb_error_name(r), error_hint(r));
            return -1;
        }
    }
    aoa_log("  52 SEND_IDENT   OK   已发送 6 项标识");

    if (!do_start) {
        aoa_log("  已跳过 53 START_ACCESSORY");
        return 0;
    }

    r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_START_ACCESSORY, 0, 0, NULL, 0,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        aoa_log("  53 START_ACCESSORY 失败：%s %s", libusb_error_name(r), error_hint(r));
        return -1;
    }
    aoa_log("  53 START_ACCESSORY OK   USB 即将逻辑插拔，等待重新枚举…");
    return 0;
}

int aoa_handshake(struct aoa_device *d, int do_start)
{
    int rc = libusb_claim_interface(d->handle, 0);

    if (rc != 0) {
        aoa_log("接口 0 claim 失败：%s %s", libusb_error_name(rc), error_hint(rc));
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
                aoa_log("  第 %d 次尝试：已进入 accessory 模式 %04x:%04x", attempt, d.vid, d.pid);
                *out = d;
                return 0;
            }
            aoa_log("  第 %d 次尝试：找到 %04x:%04x，但还不是 accessory 模式", attempt, d.vid, d.pid);
            aoa_close(&d);
        } else {
            aoa_log("  第 %d 次尝试：未发现可打开的设备", attempt);
        }
    }
    aoa_log("重试 %d 次后仍未进入 accessory 模式", RETRY_ACCESSORY_MODE);
    return -1;
}

/* 等原设备节点失效（USB 逻辑插拔完成）。
 * 前后 VID:PID 可能相同，无法据此区分新旧节点，故以「总线地址上不再出现同一节点」为判据。 */
static void wait_rebind(unsigned short vid, unsigned short pid, unsigned char old_addr)
{
    int i;

    for (i = 0; i < RETRY_REBIND; i++) {
        libusb_device **list = NULL;
        ssize_t n, k;
        int still_there = 0;

        if (aoa_stop) {
            return;
        }
        n = libusb_get_device_list(g_ctx, &list);
        for (k = 0; k < n; k++) {
            struct libusb_device_descriptor d;

            if (libusb_get_device_descriptor(list[k], &d) != 0) {
                continue;
            }
            if (d.idVendor == vid && d.idProduct == pid &&
                libusb_get_device_address(list[k]) == old_addr) {
                still_there = 1;
                break;
            }
        }
        libusb_free_device_list(list, 1);
        if (!still_there) {
            aoa_log("  原设备节点已失效（第 %d 次检查）", i + 1);
            return;
        }
        sleep(1);
    }
    aoa_log("  警告：%d 秒内未见重枚举，仍继续", RETRY_REBIND);
}

int aoa_wait_rebind(const struct aoa_device *prev)
{
    wait_rebind(prev->vid, prev->pid, prev->address);
    return 0;
}

/* ------------------------------------------------------------------ 端点 */

/* accessory 接口 = class 0xff / subclass 0xff（adb 接口的 subclass 是 0x42，据此区分）。
 * 端点地址随配置变化（accessory 单独存在时可能是 0x81/0x02，与 adb 共存时是 0x81/0x01），
 * 必须从描述符读，不能硬编码。 */
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

int aoa_claim_accessory(struct aoa_device *d)
{
    int rc;

    if (find_accessory_endpoints(libusb_get_device(d->handle), &d->ep_in, &d->ep_out) != 0) {
        aoa_log("未找到 accessory 接口的 bulk 端点（class 0xff / subclass 0xff）");
        return -1;
    }
    rc = libusb_claim_interface(d->handle, 0);
    if (rc != 0) {
        aoa_log("接口 0 claim 失败：%s %s", libusb_error_name(rc), error_hint(rc));
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

/* ------------------------------------------------------------------ 收发 */

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

    /* AOA 头与消息体必须分为两次 bulk transfer —— 参考实现同样是两个连续的 bulkTransferOut */
    r = libusb_bulk_transfer(d->handle, d->ep_out, aoa_head, AOA_HEAD_LEN, &sent, BULK_TIMEOUT_MS);
    if (r != 0) {
        return r;
    }
    if (sent != AOA_HEAD_LEN) {
        return LIBUSB_ERROR_IO;
    }

    /* 消息体按 16KB 分片（AOA 单次 bulk 上限） */
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

int aoa_read_exact(struct aoa_device *d, unsigned char *dst, unsigned int len)
{
    unsigned int done = 0;

    while (done < len) {
        unsigned int want = len - done;
        int transferred = 0;
        int r;

        if (aoa_stop) {
            return 1;
        }
        if (want > BULK_CHUNK) {
            want = BULK_CHUNK;
        }
        r = libusb_bulk_transfer(d->handle, d->ep_in, dst + done, (int)want, &transferred,
                                 BULK_TIMEOUT_MS);
        if (r == LIBUSB_ERROR_TIMEOUT) {
            continue; /* 周期性超时，交由 aoa_stop 判断是否退出 */
        }
        if (r != 0) {
            aoa_log("USB 读取失败：%s %s", libusb_error_name(r), error_hint(r));
            return -1;
        }
        if (transferred <= 0) {
            aoa_log("USB 读取返回 0 字节");
            return -1;
        }
        done += (unsigned int)transferred;
    }
    return 0;
}
