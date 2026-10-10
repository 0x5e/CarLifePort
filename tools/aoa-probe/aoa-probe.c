/*
 * aoa-probe — CarLife AOA 通路探测（macOS / libusb）
 *
 * 目的：验证车机侧能否把 Android 手机切到 AOA accessory 模式，
 *       并确认 CarLife 的 USB 帧格式是否为 [4B channelId][4B totalLen][CarLife msg]（大端）。
 *
 * 用法：
 *   ./aoa-probe --list                    只枚举 USB 设备
 *   ./aoa-probe                           完整探测：握手 → 重新枚举 → 读帧 10 秒
 *   ./aoa-probe --handshake-only          只发 51/52，不发 53（不改动手机的 USB 状态）
 *   ./aoa-probe --read 0                  握手成功后不读帧
 *   ./aoa-probe --read 30                 读帧 30 秒
 *   ./aoa-probe --vid 0x2717 --pid 0x1234  指定目标设备（默认自动挑选）
 *
 * 运行前请执行 `adb kill-server`：adb server 会通过 libusb 持有 USB 设备，
 * 导致本程序 libusb_claim_interface 失败。
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

/* 取自 CarLife-Android-Vehicle/CommonParams.java，均为握手阶段最可能出现的命令 */
static const struct known_id kKnownId[] = {
    {0x00018001UL, "MSG_CMD_HU_PROTOCOL_VERSION"},
    {0x00010002UL, "MSG_CMD_PROTOCOL_VERSION_MATCH_STATUS"},
    {0x00018003UL, "MSG_CMD_HU_INFO"},
    {0x00010004UL, "MSG_CMD_MD_INFO"},
    {0x00018005UL, "MSG_CMD_HU_BT_OOB_INFO"},
    {0x00010006UL, "MSG_CMD_MD_BT_OOB_INFO"},
    {0x00018007UL, "MSG_CMD_VIDEO_ENCODER_INIT"},
    {0x00010008UL, "MSG_CMD_VIDEO_ENCODER_INIT_DONE"},
    {0x00018009UL, "MSG_CMD_VIDEO_ENCODER_START"},
};

struct opts {
    int list_only;
    int handshake_only;
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

/* 手机厂商关键字：多数手机 iProduct 只是机型名（如 "Pixel 8"），靠厂商串识别更可靠 */
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

/* 设备角色判定标签（用于列表输出与自动挑选） */
static const char *tag_for(const struct libusb_device_descriptor *d, const struct devnames *n)
{
    if (is_accessory_pid(d->idVendor, d->idProduct)) {
        return "已进入 accessory 模式";
    }
    if (is_apple(d->idVendor)) {
        return d->idProduct == 0x12A8 ? "跳过：iPhone" : "跳过：Apple 其它设备";
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
        printf("枚举 USB 设备失败：%s\n", libusb_error_name((int)n));
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
            strcpy(nm.prod, "(无法打开：被其它程序占用？)");
        }
        printf("  %04x:%04x  %-26s %-12s %s\n", d.idVendor, d.idProduct,
               nm.prod[0] ? nm.prod : "(无名称)", nm.maker[0] ? nm.maker : "-", tag_for(&d, &nm));
    }
    libusb_free_device_list(list, 1);
}

static const char *error_hint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
        return "（设备被占用：先执行 adb kill-server，或拔掉并关闭 MTP/文件传输类程序）";
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return "（设备已消失：手机可能刚重新枚举，重跑一次即可）";
    }
    return "";
}

static void describe_config(libusb_device *dev)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, j;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        printf("  无法读取配置描述符\n");
        return;
    }
    for (i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        if (itf->num_altsetting <= 0) {
            continue;
        }
        const struct libusb_interface_descriptor *alt = &itf->altsetting[0];
        printf("  接口 %u：class=0x%02x subclass=0x%02x 端点 %u 个\n",
               alt->bInterfaceNumber, alt->bInterfaceClass, alt->bInterfaceSubClass,
               alt->bNumEndpoints);
        for (j = 0; j < alt->bNumEndpoints; j++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[j];
            const char *dir = (ep->bEndpointAddress & 0x80) ? "IN " : "OUT";
            printf("    端点[%d] 地址=0x%02x %s 类型=%s 包长=%u\n", j, ep->bEndpointAddress, dir,
                   (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK
                       ? "bulk"
                       : "非 bulk",
                   ep->wMaxPacketSize);
        }
    }
    libusb_free_config_descriptor(cfg);
}

/* 扫描设备列表：打印全部设备，挑选最合适的一个并打开。
 * 优先级由 score_for 给出；--vid/--pid 一旦指定则只认精确匹配。
 * 返回句柄（失败为 NULL）；列表在本函数内释放，返回的句柄仍有效
 * （libusb_open 内部已持有设备引用）。 */
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
        printf("枚举 USB 设备失败：%s\n", libusb_error_name((int)n));
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
            strcpy(nm.prod, "(无法打开)");
        }

        if (verbose) {
            printf("  %04x:%04x  %-26s %-12s %s\n", d.idVendor, d.idProduct,
                   nm.prod[0] ? nm.prod : "(无名称)", nm.maker[0] ? nm.maker : "-",
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
        printf("  51 GET_PROTOCOL 失败：%s %s\n", libusb_error_name(r), error_hint(r));
        return -1;
    }
    version = ((unsigned int)ver[1] << 8) | (unsigned int)ver[0];
    printf("  51 GET_PROTOCOL  OK   设备报告 AOA 版本 = %u\n", version);
    if (version < 1 || version > 2) {
        printf("  该设备不支持 AOA 1.0/2.0，无法继续\n");
        return -1;
    }

    for (i = 0; i < 6; i++) {
        unsigned char sbuf[64];
        size_t len = strlen(kIdent[i]);
        memcpy(sbuf, kIdent[i], len);
        r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_SEND_IDENT, 0, (unsigned short)i,
                                    sbuf, (unsigned short)len, EP0_TIMEOUT_MS);
        if (r < 0) {
            printf("  52 SEND_IDENT[index=%d] 失败：%s %s\n", i, libusb_error_name(r),
                    error_hint(r));
            return -1;
        }
        printf("  52 SEND_IDENT   OK   index=%d = \"%s\"\n", i, kIdent[i]);
    }

    if (!do_start) {
        printf("  已跳过 53 START_ACCESSORY（--handshake-only）\n");
        return 0;
    }

    r = libusb_control_transfer(h, REQ_TYPE_OUT_VENDOR, AOA_START_ACCESSORY, 0, 0, NULL, 0,
                                EP0_TIMEOUT_MS);
    if (r < 0) {
        printf("  53 START_ACCESSORY 失败：%s %s\n", libusb_error_name(r), error_hint(r));
        return -1;
    }
    printf("  53 START_ACCESSORY OK  USB 即将逻辑插拔，等待重新枚举…\n");
    return 0;
}

/* 等待手机以 accessory 模式重新出现 */
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
                printf("  第 %d 次尝试：已进入 accessory 模式 %04x:%04x %s\n", attempt, out->vid,
                       out->pid, out->product);
                return h;
            }
            printf("  第 %d 次尝试：找到设备 %04x:%04x，但还不是 accessory 模式\n", attempt, out->vid,
                   out->pid);
            libusb_close(h);
            empty_rounds++;
        } else {
            printf("  第 %d 次尝试：未发现可打开的设备\n", attempt);
            empty_rounds++;
        }
        if (empty_rounds >= RETRY_RESCAN + RETRY_ACCESSORY_MODE) {
            break;
        }
    }
    printf("  重试 %d 次后仍未进入 accessory 模式\n", RETRY_ACCESSORY_MODE);
    return NULL;
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

/* 读满 len 字节；返回 0 成功，1 表示时间窗口结束，-1 表示错误 */
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
            printf("  bulk 读取失败：%s %s\n", libusb_error_name(r), error_hint(r));
            return -1;
        }
        if (transferred <= 0) {
            printf("  bulk 读取返回 0 字节\n");
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
    printf("[帧 %lu] channel=%u(%s) totalLen=%u\n", index, ch, name, len);
    if (ch == 1) {
        unsigned long sid = be32(p + 4);
        const char *kn = lookup_id(sid);
        printf("        消息头 8B:");
        print_hex(p, 8);
        printf("   serviceType=0x%08lx%s%s\n", sid, kn ? " → " : "", kn ? kn : "");
    } else if (ch == 6) {
        printf("        消息头 8B:");
        print_hex(p, 8);
        printf("\n");
    } else {
        printf("        消息头 12B:");
        print_hex(p, 12);
        printf("   body %u B\n", len > 12 ? len - 12 : 0);
    }
}

static int read_frames(libusb_device_handle *h, unsigned char ep_in, int seconds)
{
    time_t deadline = time(NULL) + seconds;
    unsigned long frames = 0;
    unsigned long per_ch_count[CHANNEL_COUNT + 1] = {0};
    unsigned long long per_ch_bytes[CHANNEL_COUNT + 1] = {0};
    unsigned long long total_bytes = 0;
    int printed = 0;
    time_t last_report = time(NULL);
    int rc = libusb_claim_interface(h, 0);
    int exit_code = 0;

    if (rc != 0) {
        printf("  接口 claim 失败：%s %s\n", libusb_error_name(rc), error_hint(rc));
        return -1;
    }
    printf("  已 claim 接口 0，开始读帧（%d 秒）…\n", seconds);

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
            printf("  帧头非法：channel=%u totalLen=%u（期望 channel 1..6、len 8..64MB）\n", ch,
                    len);
            exit_code = -1;
            break;
        }
        if (ensure_buf(len) != 0) {
            printf("  分配 %u 字节缓冲失败\n", len);
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
            printf("  … 已累计 %lu 帧 / %llu B（cmd=%lu video=%lu audio=%lu tts=%lu vr=%lu touch=%lu）\n",
                   frames, total_bytes, per_ch_count[1], per_ch_count[2], per_ch_count[3],
                   per_ch_count[4], per_ch_count[5], per_ch_count[6]);
        }
    }

    printf("  读帧结束：共 %lu 帧 / %llu B；分通道帧数 cmd=%lu video=%lu audio=%lu tts=%lu vr=%lu touch=%lu\n",
           frames, total_bytes, per_ch_count[1], per_ch_count[2], per_ch_count[3], per_ch_count[4],
           per_ch_count[5], per_ch_count[6]);
    if (frames == 0) {
        printf("  未收到任何帧：手机端 CarLife 可能未启动或未同意 USB 授权弹窗\n");
    }
    libusb_release_interface(h, 0);
    return exit_code;
}

static void usage(const char *argv0)
{
    printf("用法：%s [选项]\n", argv0);
    printf("  --list                 只枚举 USB 设备，不触碰任何设备\n");
    printf("  --handshake-only       只发 51/52，不发 53（不改动手机 USB 状态）\n");
    printf("  --read <秒>            进入 accessory 模式后读帧时长，默认 10，0 表示不读\n");
    printf("  --vid <0xXXXX>         指定目标设备的 VID\n");
    printf("  --pid <0xXXXX>         指定目标设备的 PID\n");
    printf("  -h, --help             显示本帮助\n");
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
            printf("未知参数：%s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    rc = libusb_init(&ctx);
    if (rc != 0) {
        printf("libusb_init 失败：%s\n", libusb_error_name(rc));
        return 1;
    }

    printf("== 1. 枚举 USB 设备 ==\n");
    if (o.list_only) {
        list_devices(ctx);
        goto out;
    }
    h = open_target(ctx, &o, &pk, &score, 1);
    if (h == NULL) {
        printf("未找到可用的目标设备：请确认手机已插上，且没有其它程序占用\n"
               "  若手机已插上，先执行 adb kill-server（adb server 会通过 libusb 持有设备）\n"
               "  也可用 --vid/--pid 显式指定设备\n");
        exit_code = 1;
        goto out;
    }
    if (score < 2 && o.want_vid < 0 && o.want_pid < 0) {
        printf("未识别出 Android 手机，不会对该设备贸然发送 AOA 请求。\n"
               "  自动挑选到的设备是 %04x:%04x %s\n"
               "  如确认无误，请显式指定：--vid 0x%04x --pid 0x%04x\n",
               pk.vid, pk.pid, pk.product, pk.vid, pk.pid);
        exit_code = 3;
        goto close_handle;
    }
    printf("选中：%04x:%04x  %s%s\n", pk.vid, pk.pid, pk.product,
           pk.is_accessory ? "  [已是 accessory 模式]" : "");

    printf("== 2. 设备描述符 ==\n");
    describe_config(libusb_get_device(h));

    if (pk.is_accessory) {
        printf("== 3. 握手 ==\n  设备已处于 accessory 模式，跳过 51/52/53\n");
    } else {
        printf("== 3. AOA 握手 ==\n");
        rc = libusb_claim_interface(h, 0);
        if (rc != 0) {
            printf("  接口 0 claim 失败：%s %s\n", libusb_error_name(rc), error_hint(rc));
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
            printf("  完成（--handshake-only，不改变手机状态）\n");
            goto out;
        }

        printf("== 4. 等待 accessory 模式重新枚举 ==\n");
        h = wait_accessory(ctx, &o, &pk);
        if (h == NULL) {
            printf("结果：AOA 握手已发出，但设备未以 accessory 模式重新出现\n");
            exit_code = 1;
            goto out;
        }
        printf("== 5. accessory 设备描述符 ==\n");
        describe_config(libusb_get_device(h));
    }

    if (o.read_seconds > 0) {
        unsigned char ep_in = 0x81;
        printf("== 6. 读帧验证 ==\n");
        if (read_frames(h, ep_in, o.read_seconds) != 0) {
            exit_code = 1;
        }
    } else {
        printf("== 6. 读帧验证 ==\n  已跳过（--read 0）\n");
    }

close_handle:
    if (h != NULL) {
        libusb_close(h);
    }
out:
    free(g_buf);
    libusb_exit(ctx);
    printf("退出码 %d\n", exit_code);
    return exit_code;
}
