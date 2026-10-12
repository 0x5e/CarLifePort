/*
 * bridge.c — the USB half of the link process
 *
 * Structure: phone <-USB(AOA)-> this process <-TCP(127.0.0.1)-> CarLifeVehicleLib
 *
 * On USB, the 6 channels share a single bulk channel, each frame carrying an 8B head
 * [4B channelId][4B totalLen] (big-endian).
 * Locally, each channel gets a TCP listening port matching CarLifeVehicleLib's
 * CMD_SOCKET_PORT_HU series:
 *
 *   channel 1 cmd   -> 127.0.0.1:7200
 *   channel 2 video -> 127.0.0.1:8200
 *   channel 3 audio -> 127.0.0.1:9200
 *   channel 4 tts   -> 127.0.0.1:9201
 *   channel 5 vr    -> 127.0.0.1:9202
 *   channel 6 touch -> 127.0.0.1:9300
 *
 * A raw CarLife message (no AOA head) runs over the socket, matching the library's behavior
 * in ADB mode: the library sends "message head + message body" as two writes, so this
 * process must reassemble packets using the length field in the message head.
 * Message head (all big-endian):
 *   cmd(1) / touch(6)            : [2B bodyLen][2B reserved][4B serviceType]           total 8B
 *   video(2)/audio(3)/tts(4)/vr(5): [4B bodyLen][4B timestamp][4B serviceType]         total 12B
 * See packageHeadAnalysis() / setPackageHeadDataSize() / setPackageHeadType() in
 * CarLife-Vehicle-Lib/LibSource/core/CTranRecvPackageProcess.cpp.
 *
 * This is the USB half of carlife-link; main.cpp is the protocol stack half. The two used
 * to be separate processes started in order (transport first, so its ports exist before the
 * library connects). They are one process now, and that order is bridge_start() followed by
 * the library setup in main().
 *
 * Device lifetime: the phone may be plugged in before or after the process starts, so
 * bridge_start() waits for a device instead of giving up. The process still exits once a
 * link is lost (USB read error); carlifed restarts it, because neither CarLifeVehicleLib
 * nor the phone side can resume a session that was interrupted by a re-plug.
 */
#include "aoa.h"
#include "bridge.h"

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
    int client_fd; /* -1 = no client */
    char peer[64];
    pthread_mutex_t lock; /* guards client_fd / peer */
    unsigned long long rx_frames; /* local -> USB */
    unsigned long long rx_bytes;
    unsigned long long tx_frames; /* USB -> local */
    unsigned long long tx_bytes;
    unsigned long long dropped;   /* received on USB but no local client connected */
};

static struct channel g_ch[AOA_CHANNEL_COUNT + 1]; /* index 1..6 */
static struct aoa_device g_dev;
static pthread_mutex_t g_usb_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_quiet = 0;
static volatile int g_busy_threads = 0;

struct rx_arg {
    struct channel *ch;
    int fd;
};

/* ------------------------------------------------------------------ thread count */

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
        aoa_log_warn("still %d worker thread(s) not exited, continuing shutdown", g_busy_threads);
    }
}

/* ------------------------------------------------------------------ CarLife message head */

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

/* Per-channel message body caps, from *_DATA_SIZE in CTranRecvPackageProcess.h */
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

/* ------------------------------------------------------------------ socket send/receive */

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
            return -1; /* peer closed */
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

/* ------------------------------------------------------------------ local -> USB */

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

/* One thread per channel: read "message head + message body" from the socket into one full
 * message, then send it to USB */
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
            aoa_log_warn("%s: message body length %u exceeds the cap %u, dropping this connection", ch->name, body,
                    carlife_max_body(ch->id));
            report_disconnect = 0;
            break;
        }
        need = head_len + body;
        if (ensure_cap(&buf, &cap, need != 0 ? need : 1) != 0) {
            aoa_log_error("%s: failed to allocate %u-byte buffer", ch->name, need);
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
            aoa_log_error("%s: send to USB failed: %s", ch->name, libusb_error_name(r));
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
            aoa_log("%s: client disconnected", ch->name);
        }
    } else {
        pthread_mutex_unlock(&ch->lock);
    }
    close(fd);

    free(buf);
    thread_leave();
    return NULL;
}

/* ------------------------------------------------------------------ accept connections */

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
        aoa_log("%s: new connection %s replaces the old one", ch->name, ch->peer);
        shutdown(old, SHUT_RDWR); /* the old rx thread will exit and close on its own */
    } else {
        aoa_log("%s: client connected %s", ch->name, ch->peer);
    }

    arg = (struct rx_arg *)malloc(sizeof(*arg));
    if (arg == NULL) {
        aoa_log_error("%s: failed to allocate thread argument", ch->name);
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
        aoa_log_error("%s: failed to create receive thread", ch->name);
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

/* ------------------------------------------------------------------ USB -> local */

static int usb_rx_loop(void)
{
    unsigned char aoa_head[AOA_HEAD_LEN];
    unsigned char *buf = NULL;
    size_t cap = 0;
    time_t last_report = 0;
    unsigned long long last_tx = 0, last_rx = 0, last_drop = 0;

    aoa_log("start forwarding (USB -> local / local -> USB), Ctrl-C to stop");
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
            aoa_log_error("invalid AOA frame head: channel=%u totalLen=%u", ch_id, len);
            return -1;
        }
        if (ensure_cap(&buf, &cap, len) != 0) {
            aoa_log_error("failed to allocate %u-byte buffer", len);
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
            aoa_log_warn("%s: write to local socket failed, dropping this connection", ch->name);
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
                aoa_log_debug("USB->local %llu frames | local->USB %llu frames | dropped (no client) %llu frames", tx_sum,
                              rx_sum, drop_sum);
                for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
                    if (g_ch[i].tx_frames == 0 && g_ch[i].rx_frames == 0 && g_ch[i].dropped == 0) {
                        continue;
                    }
                    aoa_log_debug("    %-5s ↓%llu/%llu B  ↑%llu/%llu B%s", g_ch[i].name, g_ch[i].tx_frames,
                                  g_ch[i].tx_bytes, g_ch[i].rx_frames, g_ch[i].rx_bytes,
                                  g_ch[i].client_fd < 0 ? "  (no client)" : "");
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ startup / shutdown */

static int setup_listeners(const char *host)
{
    int i;

    for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
        struct channel *ch = &g_ch[i];
        struct sockaddr_in sa;
        int fd, on = 1;

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            aoa_log_error("%s: socket creation failed: %s", ch->name, strerror(errno));
            return -1;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(ch->port);
        if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
            aoa_log_error("invalid listen address: %s", host);
            close(fd);
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            aoa_log_error("%s: bind %s:%u failed: %s", ch->name, host, ch->port, strerror(errno));
            close(fd);
            return -1;
        }
        if (listen(fd, 1) != 0) {
            aoa_log_error("%s: listen failed: %s", ch->name, strerror(errno));
            close(fd);
            return -1;
        }
        ch->listen_fd = fd;
        aoa_log("  listening on %s:%u  (%s)", host, ch->port, ch->name);
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

/* Acquire a claimed accessory device: pick a device, send 51/52/53 unless it is already in
 * accessory mode, wait for the accessory re-enumeration, then claim the bulk interface.
 *
 * Every step is retried rather than treated as fatal, because the phone may still be on its
 * way in: it can be plugged in after this process started, or it can be held by another
 * application that has to let go first. The first attempt prints the device list; the
 * "still waiting" lines are printed once so that a phone that is not there yet does not
 * scroll the log.
 *
 * Returns 0 with *out claimed and open, or -1 if a stop was requested. */
static int acquire_device(int vid, int pid, struct aoa_device *out)
{
    unsigned int attempt = 0;
    int no_device_logged = 0;
    int not_ready_logged = 0;
    int device_seen = 0;

    for (;;) {
        struct aoa_device dev;
        int is_accessory = 0;

        if (aoa_stop) {
            return -1;
        }

        if (aoa_open_target(vid, pid, &dev, &is_accessory, attempt == 0) != 0) {
            /* Said once: the phone can stay away for minutes, and repeating this would
             * scroll the log while nothing changes. */
            if (!no_device_logged) {
                aoa_log("no phone yet, waiting: plug it in and make sure no other "
                        "application is using the USB device");
                no_device_logged = 1;
            }
            goto retry;
        }

        device_seen = 1;
        aoa_log("found %04x:%04x%s", dev.vid, dev.pid,
                is_accessory ? "  [already in accessory mode]" : "  [normal mode]");

        if (is_accessory) {
            aoa_log("  already in accessory mode, skipping 51/52/53");
            aoa_log("  note: the phone keeps an accessory session across a replug, so the CarLife app");
            aoa_log("  may still be attached to the previous one. If no traffic appears, clear the mode");
            aoa_log("  (adb shell svc usb setFunctions) and replug.");
        } else {
            if (aoa_handshake(&dev, 1) != 0) {
                aoa_close(&dev);
                goto retry;
            }
            aoa_close(&dev);
            if (aoa_wait_accessory(vid, pid, &dev) != 0) {
                aoa_log_warn("  51/52/53 were accepted, but the device did not reappear in accessory mode");
                goto retry;
            }
        }

        if (aoa_claim_accessory(&dev) != 0) {
            aoa_close(&dev);
            goto retry;
        }
        aoa_log("accessory endpoints IN=0x%02x OUT=0x%02x", dev.ep_in, dev.ep_out);
        *out = dev;
        return 0;

    retry:
        /* Only a device that was found but could not be used gets this line; with nothing
         * plugged in, the line above already said what is missing. */
        if (device_seen && !not_ready_logged) {
            aoa_log_warn("device not ready, retrying");
            not_ready_logged = 1;
        }
        sleep(1);
        attempt++;
    }
}

/* ------------------------------------------------------------------ public API */

/* Name and local port of each channel; the ports are CarLifeVehicleLib's
 * CMD/VIDEO/MEDIA/TTS/VR/TOUCH_SOCKET_PORT_HU values. */
static void init_channels(void)
{
    int i;
    static const struct {
        const char *name;
        unsigned short port;
    } kChannel[AOA_CHANNEL_COUNT] = {
        {"cmd", 7200}, {"video", 8200}, {"audio", 9200},
        {"tts", 9201}, {"vr", 9202},    {"touch", 9300},
    };

    for (i = 1; i <= AOA_CHANNEL_COUNT; i++) {
        g_ch[i].id = (unsigned int)i;
        g_ch[i].listen_fd = -1;
        g_ch[i].client_fd = -1;
        g_ch[i].name = kChannel[i - 1].name;
        g_ch[i].port = kChannel[i - 1].port;
        pthread_mutex_init(&g_ch[i].lock, NULL);
    }
}

int bridge_list_devices(void)
{
    if (aoa_init() != 0) {
        return 1;
    }
    aoa_log("USB device list:");
    aoa_list_devices();
    aoa_fini();
    return 0;
}

int bridge_start(const char *host, int vid, int pid, int quiet)
{
    pthread_t tid;

    g_quiet = quiet;
    init_channels();

    /* The stop flag the transport loop watches is the process's only "stop" signal, so
     * the handlers are installed here rather than in main(). */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (aoa_init() != 0) {
        return 1;
    }

    aoa_log("== 1. waiting for a phone ==");
    if (acquire_device(vid, pid, &g_dev) != 0) {
        aoa_fini();
        return 1;
    }

    aoa_log("== 2. start local channel listeners ==");
    if (setup_listeners(host) != 0) {
        shutdown_listeners();
        aoa_close(&g_dev);
        aoa_fini();
        return 1;
    }

    if (pthread_create(&tid, NULL, accept_loop, NULL) != 0) {
        aoa_log_error("failed to create accept thread");
        shutdown_listeners();
        aoa_close(&g_dev);
        aoa_fini();
        return 1;
    }
    pthread_detach(tid);

    aoa_log("== 3. USB link ready ==");
    return 0;
}

int bridge_loop(void)
{
    int rc = usb_rx_loop();

    if (rc != 0) {
        aoa_log_error("USB receive loop ended (error)");
    } else {
        aoa_log("USB receive loop ended (clean exit)");
    }
    return rc;
}

void bridge_shutdown(void)
{
    aoa_stop = 1;
    shutdown_listeners();
    wait_threads(3000);
    aoa_close(&g_dev);
    aoa_fini();
}
