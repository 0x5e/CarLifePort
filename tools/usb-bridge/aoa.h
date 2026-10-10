/*
 * aoa.h — CarLife 车机端 AOA（Android Open Accessory）USB 传输层
 *
 * 把 Android 手机切到 AOA accessory 模式，并在单条 USB 通道上收发 CarLife 帧。
 *
 * USB 帧结构（全大端）：
 *   [4B channelId][4B totalLen][CarLife 消息]
 * channelId 1..6 = cmd/video/audio/tts/vr/touch；
 * totalLen = CarLife 消息头长度 + 消息体长度（不含本 8B 头）。
 *
 * 本模块只负责 USB 传输，不解析 CarLife 消息语义。
 */
#ifndef CARLIFE_AOA_H
#define CARLIFE_AOA_H

#include <signal.h>

#include <libusb.h>

/* 进程级停止标志。由宿主（bridge.c）定义并在信号处理里置位，本模块只读取。 */
extern volatile sig_atomic_t aoa_stop;

#define AOA_CHANNEL_COUNT 6
#define AOA_HEAD_LEN 8

/* 一条 CarLife 消息的最大长度（video 通道上限 600KB + 12B 头） */
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

/* 带时间戳的日志，输出到 stderr */
void aoa_log(const char *fmt, ...);

int aoa_init(void);
void aoa_fini(void);

/* 枚举并打印所有 USB 设备（不触碰任何设备） */
void aoa_list_devices(void);

/* 挑选并打开目标设备，不改变其 USB 状态。
 * want_vid / want_pid 为 -1 表示自动挑选。
 * 成功返回 0；*is_accessory 表示设备是否已处于 accessory 模式。 */
int aoa_open_target(int want_vid, int want_pid, struct aoa_device *out, int *is_accessory,
                    int verbose);

/* 发送 AOA 标识请求 51/52（+ 53）。do_start=0 时不发 53，手机 USB 状态不变。
 * 内部会 claim/release 接口 0。返回 0 成功。 */
int aoa_handshake(struct aoa_device *d, int do_start);

/* 等待设备以 accessory 模式重新枚举并打开。返回 0 成功。 */
int aoa_wait_accessory(int want_vid, int want_pid, struct aoa_device *out);

/* 等 prev 描述的节点完成 USB 逻辑插拔（发过 53 之后调用）。 */
int aoa_wait_rebind(const struct aoa_device *prev);

/* 在 accessory 接口（class 0xff / subclass 0xff）上找 bulk 端点并 claim 接口。 */
int aoa_claim_accessory(struct aoa_device *d);

/* release 接口并关闭设备 */
void aoa_close(struct aoa_device *d);

/* 发送一条完整的 CarLife 消息：加 8B AOA 头、按 16KB 分片调用 bulk transfer。
 * 调用方负责串行化（同一设备不可并发调用）。返回 0 成功，否则为负的 libusb 错误码。 */
int aoa_send_message(struct aoa_device *d, unsigned int channel, const unsigned char *msg,
                     unsigned int len);

/* 读满 len 字节。返回 0 成功、1 已请求停止、-1 出错。 */
int aoa_read_exact(struct aoa_device *d, unsigned char *dst, unsigned int len);

#endif /* CARLIFE_AOA_H */
