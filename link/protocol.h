/*
 * Frame protocol between carlife-link and the Go backend.
 *
 * Transport: AF_UNIX SOCK_STREAM, with carlife-link as the listening side.
 * Frame format: [1B channel][4B payloadLen big-endian][payload]
 * The first byte of the payload is the tag, the remaining bytes are data; the
 * meaning of the tag is determined by the channel.
 *
 * Channel values match E_CHANNEL_TYPE in the CarLife protocol so they can be
 * cross-referenced with the library source.
 */
#ifndef CARLIFE_LINK_PROTOCOL_H
#define CARLIFE_LINK_PROTOCOL_H

/* Frame header length: 1B channel + 4B payloadLen (big-endian) */
#define CL_FRAME_HEADER 5

/* Maximum length of a single frame, used for defensive validation
 * (video frames are at most 600KB plus margin) */
#define CL_MAX_PAYLOAD (1024 * 1024)

enum {
    CH_CMD = 1,
    CH_VIDEO = 2,
    CH_AUDIO = 3,
    CH_TTS = 4,
    CH_VR = 5,
    CH_CTRL = 6
};

/* ---- Upstream: carlife-link -> Go backend ---- */

/* CH_CMD */
enum {
    EV_STATUS = 1,              /* [1B connected][4B connectionSetup return code big-endian] */
    EV_PROTOCOL_MATCH = 2,      /* [4B matchStatus big-endian] */
    EV_MD_INFO = 3,             /* UTF-8 JSON, phone device info */
    EV_VIDEO_INIT_DONE = 4,     /* [4B width][4B height][4B frameRate] big-endian */
    EV_VIDEO_FPS_DONE = 5,      /* [4B frameRate] big-endian */
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
    EV_AUTHEN_RESULT = 17,      /* [1B result] 1 = the phone authenticated this head unit */
    EV_GO_TO_DESKTOP = 18,
    EV_LOG = 90                 /* UTF-8 text, for diagnostics */
};

/* CH_VIDEO */
enum { EV_VIDEO_DATA = 1, EV_VIDEO_HEARTBEAT = 2 };

/* CH_AUDIO / CH_TTS / CH_VR -- audio channels share one set of tags */
enum {
    EV_AUDIO_INIT = 1,          /* [4B sampleRate][4B channelConfig][4B sampleFormat] big-endian */
    EV_AUDIO_DATA = 2,
    EV_AUDIO_STOP = 3,
    EV_AUDIO_PAUSE = 4,
    EV_AUDIO_RESUME = 5,
    EV_AUDIO_SEEK = 6
};

/* CH_CTRL */
enum { EV_CTRL_UI_ACTION_SOUND = 1 };

/* ---- Downstream: Go backend -> carlife-link ---- */

/* CH_CMD */
enum {
    CMD_PROTOCOL_VERSION = 1,       /* [4B major][4B minor] big-endian */
    CMD_VIDEO_ENCODER_INIT = 2,     /* [4B width][4B height][4B frameRate] big-endian */
    CMD_VIDEO_ENCODER_START = 3,
    CMD_VIDEO_ENCODER_PAUSE = 4,
    CMD_VIDEO_ENCODER_RESET = 5,
    CMD_GO_TO_FOREGROUND = 6,
    CMD_LAUNCH_MODE_NORMAL = 7,
    CMD_LAUNCH_MODE_PHONE = 8,
    CMD_LAUNCH_MODE_MAP = 9,
    CMD_LAUNCH_MODE_MUSIC = 10,
    CMD_AUTHEN_REQUEST = 11     /* starts the head unit authentication exchange */
};

/* CH_CTRL -- action values follow Android MotionEvent: 0=DOWN 1=UP 2=MOVE */
enum {
    CMD_TOUCH_ACTION = 1,       /* [4B action][4B x][4B y] big-endian */
    CMD_TOUCH_DOWN = 2,         /* [4B x][4B y] */
    CMD_TOUCH_UP = 3,
    CMD_TOUCH_MOVE = 4,
    CMD_TOUCH_SINGLE_CLICK = 5,
    CMD_TOUCH_DOUBLE_CLICK = 6,
    CMD_TOUCH_LONG_PRESS = 7,
    CMD_HARD_KEY = 8            /* [4B keycode] */
};

/* CH_VR */
enum { CMD_VR_DATA = 1 };       /* [4B timestamp big-endian][PCM] */

#endif /* CARLIFE_LINK_PROTOCOL_H */
