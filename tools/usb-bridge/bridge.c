/*
 * bridge.c — CarLife 车机端 USB(AOA) ↔ 本地 TCP 通道桥接
 *
 * 结构：手机 ←USB(AOA)→ 本进程 ←TCP(127.0.0.1)→ CarLifeVehicleLib
 *
 * USB 上 6 个通道复用同一条 bulk 通道，每帧带 8B 头 [4B channelId][4B totalLen]（大端）。
 * 本地为每个通道开一个 TCP 监听端口，与 CarLifeVehicleLib 的 CMD_SOCKET_PORT_HU 系列一致：
 *
 *   channel 1 cmd   → 127.0.0.1:7200
 *   channel 2 video → 127.0.0.1:8200
 *   channel 3 audio → 127.0.0.1:9200
 *   channel 4 tts   → 127.0.0.1:9201
 *   channel 5 vr    → 127.0.0.1:9202
 *   channel 6 touch → 127.0.0.1:9300
 *
 * socket 上跑的是裸 CarLife 消息（无 AOA 头），与 ADB 模式下的库行为一致：
 * 库按「消息头 + 消息体」两次 write 发送，因此本进程需要按消息头里的长度字段重新组包。
 * 消息头（全大端）：
 *   cmd(1) / touch(6)            : [2B bodyLen][2B 保留][4B serviceType]           共 8B
 *   video(2)/audio(3)/tts(4)/vr(5): [4B bodyLen][4B timestamp][4B serviceType]     共 12B
 * 见 CarLife-Vehicle-Lib/LibSource/core/CTranRecvPackageProcess.cpp 的
 * packageHeadAnalysis() / setPackageHeadDataSize() / setPackageHeadType()。
 *
 * 用法：先启动本进程（完成 AOA 握手并开始监听），再启动链接了 libcarlifevehicle 的车机程序。
 */
#include "aoa.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"

volatile sig_atomic_t aoa_stop = 0;

struct channel {
    unsigned int id;
    const char *name;
    unsigned short port;
    int listen_fd;
    int client_fd; /* -1 = 无客户端 */
    char peer[64];
    pthread_mutex_t lock; /* 保护 client_fd / peer */
    unsigned long long rx_frames; /* 本地 → USB */
    unsigned long long rx_bytes;
    unsigned long long tx_frames; /* USB → 本地 */
    unsigned long long tx_bytes;
    unsigned long long dropped;   /* USB 收到但本地未连接 */
};

static struct channel g_ch[AOA_CHANNEL_COUNT + 1]; /* 下标 1..6 */
static struct aoa_device g_dev;
static pthread_mutex_t g_usb_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_quiet = 0;
static volatile int g_busy_threads = 0;

struct rx_arg {
    struct channel *ch;
    int fd;
};

/* ------------------------------------------------------------------ 线程计数 */

static void thread_enter(void)
{
    __sync_fetch_and_add(&g_busy_threads, 1);
}

static void thread_leave(void)
{
    __sync_fetch_and_sub(&g_busy_threads, 1);
}

static void wait_threads(int timeout_ms)
{
    int waited = 0;

    while (g_busy_threads > 0 && waited < timeout_ms) {
        usleep(50 * 1000);
        waited += 50;
    }
    if (g_busy_threads > 0) {
        aoa_log("仍有 %d 个工作线程未退出，继续收尾", g_busy_threads);
    }
}

/* ------------------------------------------------------------------ CarLife 消息头 */

static unsigned int carlife_head_len(unsigned int ch)
{
    return (ch == 1 || ch == 6) ? 8u : 12u;
}

static unsigned int carlife_body_len(unsigned int ch, const unsigned char *head)
{
    if (ch == 1 || ch == 6) {
        return ((unsigned int)head[0] << 8) | (unsigned int)head[1];
    }
    return ((unsigned int)head[0] << 24) | ((unsigned int)head[1] << 16) |
           ((unsigned int)head[2] << 8) | (unsigned int)head[3];
}

/* 各通道消息体上限，取自 CTranRecvPackageProcess.h 的 *_DATA_SIZE */
static unsigned int carlife_max_body(unsigned int ch)
{
    switch (ch) {
    case 1:
        return 40u * 1024u; /* CMD_DATA_SIZE */
    case 2:
        return 600u * 1024u; /* VIDEO_DATA_SIZE */
    case 3:
        return 100u * 1024u; /* MEDIA_DATA_SIZE */
    case 4:
        return 50u * 1024u; /* TTS_DATA_SIZE */
    case 5:
        return 50u * 1024u; /* VR_DATA_SIZE */
    case 6:
        return 10u * 1024u; /* CTRL_DATA_SIZE */
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ socket 收发 */

static int read_socket_full(int fd, unsigned char *dst, unsigned int len)
{
    unsigned int done = 0;

    while (done < len) {
        struct pollfd p;
        int n, r;

        if (aoa_stop) {
            return -1;
        }
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        n = poll(&p, 1, 500);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            continue;
        }
        r = (int)recv(fd, dst + done, len - done, 0);
        if (r == 0) {
            return -1; /* 对端关闭 */
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (unsigned int)r;
    }
    return 0;
}

static int write_socket_full(int fd, const unsigned char *src, unsigned int len)
{
    unsigned int done = 0;

    while (done < len) {
        ssize_t r = send(fd, src + done, len - done, MSG_NOSIGNAL);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        done += (unsigned int)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ 本地 → USB */

static int ensure_cap(unsigned char **buf, size_t *cap, size_t need)
{
    unsigned char *p;

    if (need <= *cap) {
        return 0;
    }
    p = (unsigned char *)realloc(*buf, need);
    if (p == NULL) {
        return -1;
    }
    *buf = p;
    *cap = need;
    return 0;
}

/* 每条通道一个线程：从 socket 读「消息头 + 消息体」组成一条完整消息，再发到 USB */
static void *channel_rx(void *param)
{
    struct rx_arg *arg = (struct rx_arg *)param;
    struct channel *ch = arg->ch;
    int fd = arg->fd;
    unsigned char *buf = NULL;
    size_t cap = 0;
    int report_disconnect = 1;

    free(arg);
    thread_enter();

    for (;;) {
        unsigned char head[12];
        unsigned int head_len = carlife_head_len(ch->id);
        unsigned int body, need;
        int r;

        if (read_socket_full(fd, head, head_len) != 0) {
            break;
        }
        body = carlife_body_len(ch->id, head);
        if (body > carlife_max_body(ch->id)) {
            aoa_log("%s：消息体长度 %u 超出上限 %u，断开该连接", ch->name, body,
                    carlife_max_body(ch->id));
            report_disconnect = 0;
            break;
        }
        need = head_len + body;
        if (ensure_cap(&buf, &cap, need != 0 ? need : 1) != 0) {
            aoa_log("%s：分配 %u 字节缓冲失败", ch->name, need);
            report_disconnect = 0;
            break;
        }
        memcpy(buf, head, head_len);
        if (body > 0 && read_socket_full(fd, buf + head_len, body) != 0) {
            break;
        }

        pthread_mutex_lock(&g_usb_tx_lock);
        r = aoa_send_message(&g_dev, ch->id, buf, need);
        pthread_mutex_unlock(&g_usb_tx_lock);
        if (r != 0) {
            aoa_log("%s：发送到 USB 失败：%s", ch->name, libusb_error_name(r));
            report_disconnect = 0;
            aoa_stop = 1;
            break;
        }
        ch->rx_frames++;
        ch->rx_bytes += need;
    }

    pthread_mutex_lock(&ch->lock);
    if (ch->client_fd == fd) {
        ch->client_fd = -1;
        ch->peer[0] = '\0';
        pthread_mutex_unlock(&ch->lock);
        if (report_disconnect) {
            aoa_log("%s：客户端已断开", ch->name);
        }
    } else {
        pthread_mutex_unlock(&ch->lock);
    }
    close(fd);

    free(buf);
    thread_leave();
    return NULL;
}

/* ------------------------------------------------------------------ 接受连接 */

static void accept_one(struct channel *ch)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    struct rx_arg *arg;
    pthread_t tid;
    int fd, old, on = 1;

    fd = accept(ch->listen_fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
        return;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    pthread_mutex_lock(&ch->lock);
    old = ch->client_fd;
    ch->client_fd = fd;
    snprintf(ch->peer, sizeof(ch->peer), "%s:%u", inet_ntoa(sa.sin_addr),
             (unsigned)ntohs(sa.sin_port));
    pthread_mutex_unlock(&ch->lock);

    if (old >= 0) {
        aoa_log("%s：新连接 %s 取代旧连接", ch->name, ch->peer);
        shutdown(old, SHUT_RDWR); /* 旧 rx 线程会自行退出并 close */
    } else {
        aoa_log("%s：客户端已连接 %s", ch->name, ch->peer);
    }

    arg = (struct rx_arg *)malloc(sizeof(*arg));
    if (arg == NULL) {
        aoa_log("%s：分配线程参数失败", ch->name);
        pthread_mutex_lock(&ch->lock);
        ch->client_fd = -1;
        ch->peer[0] = '\0';
        pthread_mutex_unlock(&ch->lock);
        close(fd);
        return;
    }
    arg->ch = ch;
    arg->fd = fd;
    if (pthread_create(&tid, NULL, channel_rx, arg) != 0) {
        aoa_log("%s：创建接收线程失败", ch->name);
        pthread_mutex_lock(&ch->lock);
        ch->client_fd = -1;
        ch->peer[0] = '\0';
        pthread_mutex_unlock(&ch->lock);
        close(fd);
        free(arg);
        return;
    }
    pthread_detach(tid);
}

static void *accept_loop(void *unused)
{
    struct pollfd pfd[AOA_CHANNEL_COUNT];
    int i;

    (void)unused;
    thread_enter();
    for (i = 0; i < AOA_CHANNEL_COUNT; i++) {
        pfd[i].fd = g_ch[i + 1].listen_fd;
        pfd[i].events = POLLIN;
    }
    while (!aoa_stop) {
        int n;

        for (i = 0; i < AOA_CHANNEL_COUNT; i++) {
            pfd[i].revents = 0;
        }
        n = poll(pfd, AOA_CHANNEL_COUNT, 500);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (i = 0; i < AOA_CHANNEL_COUNT; i++) {
            if (pfd[i].revents & POLLIN) {
                accept_one(&g_ch[i + 1]);
            }
        }
    }
    thread_leave();
    return NULL;
}

/* ------------------------------------------------------------------ USB → 本地 */

static int usb_rx_loop(void)
{
    unsigned char aoa_head[AOA_HEAD_LEN];
    unsigned char *buf = NULL;
    size_t cap = 0;
    time_t last_report = 0;
    unsigned long long last_tx = 0, last_rx = 0, last_drop = 0;

    aoa_log("开始转发（USB → 本地 / 本地 → USB），Ctrl-C 结束");
    if (!g_quiet) {
        time(&last_report);
    }

    for (;;) {
        unsigned int ch_id, len, head_total;
        struct channel *ch;
        int fd, st;
        unsigned long long tx_sum = 0, rx_sum = 0, drop_sum = 0;
        int i;

        st = aoa_read_exact(&g_dev, aoa_head, AOA_HEAD_LEN);
        if (st == 1) {
            return 0;
        }
        if (st < 0) {
            return -1;
        }
        ch_id = ((unsigned int)aoa_head[0] << 24) | ((unsigned int)aoa_head[1] << 16) |
                ((unsigned int)aoa_head[2] << 8) | (unsigned int)aoa_head[3];
        len = ((unsigned int)aoa_head[4] << 24) | ((unsigned int)aoa_head[5] << 16) |
              ((unsigned int)aoa_head[6] << 8) | (unsigned int)aoa_head[7];
        head_total = carlife_head_len(ch_id);
        if (ch_id < 1 || ch_id > AOA_CHANNEL_COUNT || len < head_total || len > AOA_MSG_MAX) {
            aoa_log("非法 AOA 帧头：channel=%u totalLen=%u", ch_id, len);
            return -1;
        }
        if (ensure_cap(&buf, &cap, len) != 0) {
            aoa_log("分配 %u 字节缓冲失败", len);
            return -1;
        }
        st = aoa_read_exact(&g_dev, buf, len);
        if (st == 1) {
            return 0;
        }
        if (st < 0) {
            return -1;
        }

        ch = &g_ch[ch_id];
        ch->tx_frames++;
        ch->tx_bytes += len;

        pthread_mutex_lock(&ch->lock);
        fd = ch->client_fd;
        pthread_mutex_unlock(&ch->lock);
        if (fd < 0) {
            ch->dropped++;
            continue;
        }
        if (write_socket_full(fd, buf, len) != 0) {
            aoa_log("%s：写入本地 socket 失败，断开该连接", ch->name);
            pthread_mutex_lock(&ch->lock);
            if (ch->client_fd == fd) {
                shutdown(fd, SHUT_RDWR);
            }
            pthread_mutex_unlock(&ch->lock);
        }

        if (g_quiet) {
            continue;
        }
        for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
            tx_sum += g_ch[i].tx_frames;
            rx_sum += g_ch[i].rx_frames;
            drop_sum += g_ch[i].dropped;
        }
        if (time(NULL) != last_report) {
            last_report = time(NULL);
            if (tx_sum != last_tx || rx_sum != last_rx || drop_sum != last_drop) {
                last_tx = tx_sum;
                last_rx = rx_sum;
                last_drop = drop_sum;
                aoa_log("USB→本地 %llu 帧 ｜ 本地→USB %llu 帧 ｜ 未连接丢弃 %llu 帧", tx_sum, rx_sum,
                        drop_sum);
                for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
                    if (g_ch[i].tx_frames == 0 && g_ch[i].rx_frames == 0 && g_ch[i].dropped == 0) {
                        continue;
                    }
                    aoa_log("    %-5s ↓%llu/%llu B  ↑%llu/%llu B%s", g_ch[i].name, g_ch[i].tx_frames,
                            g_ch[i].tx_bytes, g_ch[i].rx_frames, g_ch[i].rx_bytes,
                            g_ch[i].client_fd < 0 ? "  (未连接)" : "");
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ 启动 / 收尾 */

static int setup_listeners(const char *host)
{
    int i;

    for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
        struct channel *ch = &g_ch[i];
        struct sockaddr_in sa;
        int fd, on = 1;

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            aoa_log("%s：创建 socket 失败：%s", ch->name, strerror(errno));
            return -1;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(ch->port);
        if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
            aoa_log("无效的监听地址：%s", host);
            close(fd);
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            aoa_log("%s：绑定 %s:%u 失败：%s", ch->name, host, ch->port, strerror(errno));
            close(fd);
            return -1;
        }
        if (listen(fd, 1) != 0) {
            aoa_log("%s：listen 失败：%s", ch->name, strerror(errno));
            close(fd);
            return -1;
        }
        ch->listen_fd = fd;
        aoa_log("  监听 %s:%u  （%s）", host, ch->port, ch->name);
    }
    return 0;
}

static void shutdown_listeners(void)
{
    int i;

    for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
        struct channel *ch = &g_ch[i];

        if (ch->listen_fd >= 0) {
            close(ch->listen_fd);
            ch->listen_fd = -1;
        }
        pthread_mutex_lock(&ch->lock);
        if (ch->client_fd >= 0) {
            shutdown(ch->client_fd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&ch->lock);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    aoa_stop = 1;
}

static void usage(const char *argv0)
{
    printf("用法：%s [选项]\n", argv0);
    printf("  --list                 只枚举 USB 设备，不触碰任何设备\n");
    printf("  --force-handshake      当前已在 accessory 模式时仍重发 51/52/53\n");
    printf("  --vid <0xXXXX>         指定目标设备的 VID\n");
    printf("  --pid <0xXXXX>         指定目标设备的 PID\n");
    printf("  --host <地址>          本地监听地址，默认 %s\n", DEFAULT_HOST);
    printf("  --quiet                不输出周期性统计\n");
    printf("  -h, --help             显示本帮助\n");
}

static int parse_args(int argc, char **argv, int *vid, int *pid, const char **host,
                      int *force_handshake, int *list_only)
{
    int i;

    *vid = -1;
    *pid = -1;
    *host = DEFAULT_HOST;
    *force_handshake = 0;
    *list_only = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            *list_only = 1;
        } else if (strcmp(argv[i], "--force-handshake") == 0) {
            *force_handshake = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            *vid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            *pid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            *host = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            aoa_log("未知参数：%s", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int vid, pid, force_handshake, list_only;
    const char *host;
    int is_accessory = 0;
    int rc;
    pthread_t tid;
    int i;

    if (parse_args(argc, argv, &vid, &pid, &host, &force_handshake, &list_only) != 0) {
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
        g_ch[i].id = (unsigned int)i;
        g_ch[i].listen_fd = -1;
        g_ch[i].client_fd = -1;
        pthread_mutex_init(&g_ch[i].lock, NULL);
    }
    g_ch[1].name = "cmd";
    g_ch[1].port = 7200;
    g_ch[2].name = "video";
    g_ch[2].port = 8200;
    g_ch[3].name = "audio";
    g_ch[3].port = 9200;
    g_ch[4].name = "tts";
    g_ch[4].port = 9201;
    g_ch[5].name = "vr";
    g_ch[5].port = 9202;
    g_ch[6].name = "touch";
    g_ch[6].port = 9300;

    if (aoa_init() != 0) {
        return 1;
    }
    if (list_only) {
        aoa_log("USB 设备列表：");
        aoa_list_devices();
        aoa_fini();
        return 0;
    }

    aoa_log("== 1. 枚举并挑选设备 ==");
    rc = aoa_open_target(vid, pid, &g_dev, &is_accessory, !g_quiet);
    if (rc != 0) {
        aoa_log("未找到可用设备：请确认手机已插上，且没有其它程序占用 USB（如 adb server、微信）");
        aoa_fini();
        return 1;
    }
    aoa_log("选中 %04x:%04x%s", g_dev.vid, g_dev.pid,
            is_accessory ? "  [已处于 accessory 模式]" : "  [普通模式]");

    if (is_accessory && !force_handshake) {
        aoa_log("== 2. AOA 握手 ==\n  设备已处于 accessory 模式，跳过 51/52/53");
    } else {
        struct aoa_device prev = g_dev;

        aoa_log("== 2. AOA 握手 ==");
        if (force_handshake && is_accessory) {
            aoa_log("  --force-handshake：重发 51/52/53 覆盖手机上残留的描述符");
        }
        if (aoa_handshake(&g_dev, 1) != 0) {
            aoa_close(&g_dev);
            aoa_fini();
            return 1;
        }
        aoa_close(&g_dev);
        if (is_accessory) {
            aoa_wait_rebind(&prev);
        }
        aoa_log("== 3. 等待 accessory 模式重新枚举 ==");
        if (aoa_wait_accessory(vid, pid, &g_dev) != 0) {
            aoa_log("AOA 握手已发出，但设备未以 accessory 模式重新出现");
            aoa_fini();
            return 1;
        }
    }

    aoa_log("== 4. 准备 USB 端点 ==");
    if (aoa_claim_accessory(&g_dev) != 0) {
        aoa_close(&g_dev);
        aoa_fini();
        return 1;
    }
    aoa_log("  accessory 端点 IN=0x%02x OUT=0x%02x", g_dev.ep_in, g_dev.ep_out);

    aoa_log("== 5. 启动本地通道监听 ==");
    if (setup_listeners(host) != 0) {
        shutdown_listeners();
        aoa_close(&g_dev);
        aoa_fini();
        return 1;
    }

    if (pthread_create(&tid, NULL, accept_loop, NULL) != 0) {
        aoa_log("创建 accept 线程失败");
        shutdown_listeners();
        aoa_close(&g_dev);
        aoa_fini();
        return 1;
    }
    pthread_detach(tid);

    aoa_log("== 6. 桥接已就绪，可以启动车机程序 ==");
    rc = usb_rx_loop();

    if (rc != 0) {
        aoa_log("USB 接收循环结束（错误）");
    } else {
        aoa_log("USB 接收循环结束（正常退出）");
    }

    aoa_stop = 1;
    shutdown_listeners();
    wait_threads(3000);
    aoa_close(&g_dev);
    aoa_fini();
    return rc == 0 ? 0 : 1;
}
