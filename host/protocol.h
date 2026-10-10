/*
 * carlife-host 与 Go 后端之间的帧协议。
 *
 * 传输：AF_UNIX SOCK_STREAM，carlife-host 作为监听方。
 * 帧格式：[1B channel][4B payloadLen 大端][payload]
 * payload 的第一个字节是 tag，其余是数据；tag 的含义由 channel 决定。
 *
 * channel 取值与 CarLife 协议里的 E_CHANNEL_TYPE 一致，便于对照库源码。
 */
#ifndef CARLIFE_HOST_PROTOCOL_H
#define CARLIFE_HOST_PROTOCOL_H

/* 帧头长度：1B channel + 4B payloadLen(大端) */
#define CL_FRAME_HEADER 5

/* 默认 Unix socket 路径 */
#define CL_DEFAULT_SOCKET "/tmp/carlife-host.sock"

/* usb-bridge 命令通道的本地端口，宿主用它判断桥接层是否已经就绪 */
#define CL_BRIDGE_PORT 7200

/* 单帧最大长度，用于防御性校验（视频帧最大 600KB + 余量） */
#define CL_MAX_PAYLOAD (1024 * 1024)

enum {
    CH_CMD = 1,
    CH_VIDEO = 2,
    CH_AUDIO = 3,
    CH_TTS = 4,
    CH_VR = 5,
    CH_CTRL = 6
};

/* ---- 上行：carlife-host -> Go 后端 ---- */

/* CH_CMD */
enum {
    EV_STATUS = 1,              /* [1B connected][4B connectionSetup 返回码 大端] */
    EV_PROTOCOL_MATCH = 2,      /* [4B matchStatus 大端] */
    EV_MD_INFO = 3,             /* UTF-8 JSON，手机设备信息 */
    EV_VIDEO_INIT_DONE = 4,     /* [4B width][4B height][4B frameRate] 大端 */
    EV_VIDEO_FPS_DONE = 5,      /* [4B frameRate] 大端 */
    EV_FOREGROUND = 6,
    EV_BACKGROUND = 7,
    EV_SCREEN_ON = 8,
    EV_SCREEN_OFF = 9,
    EV_TELE_STATE = 10,         /* [1B state] 0=idle 1=incoming 2=outgoing 3=incalling */
    EV_MIC_WAKEUP = 11,
    EV_MIC_END = 12,
    EV_MIC_RECOG = 13,
    EV_MD_EXIT = 14,
    EV_MODULE_STATUS = 15,      /* UTF-8 JSON */
    EV_FEATURE_CONFIG_REQUEST = 16,
    EV_LOG = 90                 /* UTF-8 文本，诊断用 */
};

/* CH_VIDEO */
enum { EV_VIDEO_DATA = 1, EV_VIDEO_HEARTBEAT = 2 };

/* CH_AUDIO / CH_TTS / CH_VR —— 音频类通道共用一组 tag */
enum {
    EV_AUDIO_INIT = 1,          /* [4B sampleRate][4B channelConfig][4B sampleFormat] 大端 */
    EV_AUDIO_DATA = 2,
    EV_AUDIO_STOP = 3,
    EV_AUDIO_PAUSE = 4,
    EV_AUDIO_RESUME = 5,
    EV_AUDIO_SEEK = 6
};

/* CH_CTRL */
enum { EV_CTRL_UI_ACTION_SOUND = 1 };

/* ---- 下行：Go 后端 -> carlife-host ---- */

/* CH_CMD */
enum {
    CMD_PROTOCOL_VERSION = 1,       /* [4B major][4B minor] 大端 */
    CMD_VIDEO_ENCODER_INIT = 2,     /* [4B width][4B height][4B frameRate] 大端 */
    CMD_VIDEO_ENCODER_START = 3,
    CMD_VIDEO_ENCODER_PAUSE = 4,
    CMD_VIDEO_ENCODER_RESET = 5,
    CMD_GO_TO_FOREGROUND = 6,
    CMD_LAUNCH_MODE_NORMAL = 7,
    CMD_LAUNCH_MODE_PHONE = 8,
    CMD_LAUNCH_MODE_MAP = 9,
    CMD_LAUNCH_MODE_MUSIC = 10
};

/* CH_CTRL —— action 取值沿用 Android MotionEvent：0=DOWN 1=UP 2=MOVE */
enum {
    CMD_TOUCH_ACTION = 1,       /* [4B action][4B x][4B y] 大端 */
    CMD_TOUCH_DOWN = 2,         /* [4B x][4B y] */
    CMD_TOUCH_UP = 3,
    CMD_TOUCH_MOVE = 4,
    CMD_TOUCH_SINGLE_CLICK = 5,
    CMD_TOUCH_DOUBLE_CLICK = 6,
    CMD_TOUCH_LONG_PRESS = 7,
    CMD_HARD_KEY = 8            /* [4B keycode] */
};

/* CH_VR */
enum { CMD_VR_DATA = 1 };       /* [4B timestamp 大端][PCM] */

#endif /* CARLIFE_HOST_PROTOCOL_H */
