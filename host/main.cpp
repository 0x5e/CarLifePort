/*
 * carlife-host —— CarLife 车机侧协议栈的宿主进程。
 *
 * 上游是 tools/usb-bridge 在 127.0.0.1 上开的六条 TCP 通道，本进程通过
 * libcarlifevehicle 与手机完成 CarLife 协议交互；下游把六条通道的数据
 * 以带长度前缀的帧写到 AF_UNIX socket，供 Go 后端消费，并从同一 socket
 * 接收命令回写到手机。
 *
 * 进程模型：主线程负责 accept/读命令；cmd/video/audio/tts/vr 各一条接收
 * 线程跑库的阻塞式 xxxReceiveOperation()。
 */
#include "CCarLifeLibWrapper.h"
#include "protocol.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace CCarLifeLibH;

/* ------------------------------------------------------------------ 全局 */

static int g_listen_fd = -1;
static int g_client_fd = -1;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* connectionSetup() 的返回码，0 表示六条通道都已建立 */
static int g_connection_result = -1;

static int g_quiet = 0;
static int g_verbose = 0;

/* ------------------------------------------------------------- 日志与工具 */

static void log_line(const char* fmt, ...)
{
    if (g_quiet) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_now;
    time_t seconds = tv.tv_sec;
    localtime_r(&seconds, &tm_now);

    fprintf(stderr, "[%02d:%02d:%02d.%03d] ", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
            (int)(tv.tv_usec / 1000));
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static void put_be32(uint8_t* out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t* in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static std::string json_quote(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    out += '"';
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char escape[8];
                snprintf(escape, sizeof(escape), "\\u%04x", c);
                out += escape;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
    return out;
}

/* ------------------------------------------------------------ socket 输出 */

static int write_all(int fd, const uint8_t* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        // 库的头文件里有 using namespace std，必须显式用全局作用域调用系统接口，
        // 否则 bind/send 这类名字会与标准库同名函数冲突。
        ssize_t written = ::send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return 0;
        }
        offset += (size_t)written;
    }
    return 1;
}

/* 向当前客户端发一帧。分片写由发送锁串行化，避免多通道交错。 */
static void send_frame(uint8_t channel, uint8_t tag, const void* data, uint32_t len)
{
    uint8_t header[CL_FRAME_HEADER + 1];
    uint32_t payload_len = len + 1;
    header[0] = channel;
    put_be32(header + 1, payload_len);
    header[CL_FRAME_HEADER] = tag;

    pthread_mutex_lock(&g_send_lock);
    int fd = g_client_fd;
    if (fd >= 0) {
        if (!write_all(fd, header, sizeof(header)) || (len > 0 && !write_all(fd, (const uint8_t*)data, len))) {
            log_line("写入客户端失败：%s", strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_send_lock);
}

static void send_text(uint8_t tag, const std::string& text)
{
    send_frame(CH_CMD, tag, text.data(), (uint32_t)text.size());
}

/* 上报车机通道状态；客户端接入时以及状态发生变化时各发一次 */
static void broadcast_status(void)
{
    uint8_t status[5];
    status[0] = (g_connection_result == 0) ? 1 : 0;
    put_be32(status + 1, (uint32_t)g_connection_result);
    send_frame(CH_CMD, EV_STATUS, status, sizeof(status));
}

/* 等 usb-bridge 在命令通道端口上开始监听。探测连接会立刻关闭，
 * 桥接层会重新 accept，不会占用正式通道。 */
static int wait_for_bridge(int port, int timeout_seconds)
{
    for (int elapsed = 0; elapsed < timeout_seconds; ++elapsed) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in address;
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons((uint16_t)port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int ready = (::connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
            ::close(fd);
            if (ready) {
                return 1;
            }
        }
        if (elapsed % 5 == 0) {
            log_line("等待 usb-bridge 在 127.0.0.1:%d 就绪…", port);
        }
        sleep(1);
    }
    return 0;
}

/* ---------------------------------------------------- 命令通道回调（手机->车机） */

static void cb_protocol_match(S_PROTOCOL_VERSION_MATCH_SATUS* status)
{
    uint8_t buffer[4];
    put_be32(buffer, status->matchStatus);
    send_frame(CH_CMD, EV_PROTOCOL_MATCH, buffer, sizeof(buffer));
    log_line("协议版本匹配状态：%u", status->matchStatus);
}

static void cb_md_info(S_MD_INFO* info)
{
    std::string json = "{";
#define STR_FIELD(name)                       \
    json += "\"" #name "\":";                 \
    json += json_quote(info->name);           \
    json += ","
    STR_FIELD(os);
    STR_FIELD(board);
    STR_FIELD(bootloader);
    STR_FIELD(brand);
    STR_FIELD(cpu_abi);
    STR_FIELD(cpu_abi2);
    STR_FIELD(device);
    STR_FIELD(display);
    STR_FIELD(fingerprint);
    STR_FIELD(hardware);
    STR_FIELD(host);
    STR_FIELD(cid);
    STR_FIELD(manufacturer);
    STR_FIELD(model);
    STR_FIELD(product);
    STR_FIELD(serial);
    STR_FIELD(codename);
    STR_FIELD(incremental);
    STR_FIELD(release);
    STR_FIELD(sdk);
#undef STR_FIELD
    json += "\"sdk_int\":";
    json += std::to_string(info->sdk_int);
    json += ",\"btaddress\":";
    json += json_quote(info->btaddress);
    json += "}";
    send_text(EV_MD_INFO, json);
    log_line("手机设备信息：%s %s (Android %s)", info->manufacturer.c_str(), info->model.c_str(),
             info->release.c_str());
}

static void cb_md_bt_pair_info(S_BT_PAIR_INFO* info)
{
    log_line("蓝牙配对信息：name=%s status=%u", info->name.c_str(), info->status);
}

static void cb_video_encoder_init_done(S_VIDEO_ENCODER_INIT_DONE* info)
{
    uint8_t buffer[12];
    put_be32(buffer, info->width);
    put_be32(buffer + 4, info->height);
    put_be32(buffer + 8, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_INIT_DONE, buffer, sizeof(buffer));
    log_line("视频编码器就绪：%ux%u@%u", info->width, info->height, info->frameRate);
}

static void cb_video_frame_rate_done(S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE* info)
{
    uint8_t buffer[4];
    put_be32(buffer, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_FPS_DONE, buffer, sizeof(buffer));
    log_line("帧率已切换：%u", info->frameRate);
}

static void cb_tele_state(uint8_t state)
{
    send_frame(CH_CMD, EV_TELE_STATE, &state, 1);
}

static void cb_tele_incoming(void)
{
    cb_tele_state(1);
}
static void cb_tele_outgoing(void)
{
    cb_tele_state(2);
}
static void cb_tele_idle(void)
{
    cb_tele_state(0);
}
static void cb_tele_in_calling(void)
{
    cb_tele_state(3);
}

static void cb_screen_on(void)
{
    send_frame(CH_CMD, EV_SCREEN_ON, NULL, 0);
    log_line("手机屏幕点亮");
}

static void cb_screen_off(void)
{
    send_frame(CH_CMD, EV_SCREEN_OFF, NULL, 0);
    log_line("手机屏幕熄灭");
}

static void cb_screen_user_present(void)
{
    log_line("手机检测到用户在场");
}

static void cb_foreground(void)
{
    send_frame(CH_CMD, EV_FOREGROUND, NULL, 0);
    log_line("CarLife 切到前台");
}

static void cb_background(void)
{
    send_frame(CH_CMD, EV_BACKGROUND, NULL, 0);
    log_line("CarLife 切到后台");
}

static void cb_go_to_desktop(void)
{
    log_line("手机回到桌面");
}

static void cb_mic_wakeup(void)
{
    send_frame(CH_CMD, EV_MIC_WAKEUP, NULL, 0);
    log_line("语音唤醒");
}

static void cb_mic_end(void)
{
    send_frame(CH_CMD, EV_MIC_END, NULL, 0);
    log_line("语音录制结束");
}

static void cb_mic_recog(void)
{
    send_frame(CH_CMD, EV_MIC_RECOG, NULL, 0);
    log_line("语音识别开始");
}

static void cb_module_status(S_MODULE_STATUS_LIST_MOBILE* list)
{
    std::string json = "[";
    S_MODULE_STATUS_MOBILE* node = list->moduleStatus;
    for (u32 i = 0; node != NULL && i < list->cnt; ++i, node = node->next) {
        if (i != 0) {
            json += ",";
        }
        json += "{\"moduleID\":";
        json += std::to_string(node->moduleID);
        json += ",\"statusID\":";
        json += std::to_string(node->statusID);
        json += "}";
    }
    json += "]";
    send_text(EV_MODULE_STATUS, json);
}

static void cb_navi_next_turn(S_NAVI_NEXT_TURN_INFO* info)
{
    log_line("导航转向提示：action=%u nextTurn=%u road=%s", info->action, info->nextTurn, info->roadName.c_str());
}

/* 手机请求特性配置时按库自带参考实现回一份 FOCUS_UI，否则手机侧会一直等 */
static S_FEATURE_CONFIG_LIST g_feature_config_list;
static S_FEATURE_CONFIG g_feature_config_focus_ui;

static void cb_feature_config_request(void)
{
    g_feature_config_list.cnt = 1;
    g_feature_config_list.pFeatureConfig = &g_feature_config_focus_ui;
    g_feature_config_focus_ui.key = "FOCUS_UI";
    g_feature_config_focus_ui.value = 1;
    g_feature_config_focus_ui.pNext = NULL;
    CCarLifeLib::getInstance()->cmdFeatureConfigResponse(&g_feature_config_list);

    send_frame(CH_CMD, EV_FEATURE_CONFIG_REQUEST, NULL, 0);
    log_line("已回复手机的特性配置请求（FOCUS_UI=1）");
}

static void cb_md_exit(void)
{
    send_frame(CH_CMD, EV_MD_EXIT, NULL, 0);
    log_line("手机端 CarLife 退出");
}

/* ---------------------------------------------------------------- 音视频通道 */

static void send_audio_init(uint8_t channel, S_AUDIO_INIT_PARAMETER* param)
{
    uint8_t buffer[12];
    put_be32(buffer, param->sampleRate);
    put_be32(buffer + 4, param->channelConfig);
    put_be32(buffer + 8, param->sampleFormat);
    send_frame(channel, EV_AUDIO_INIT, buffer, sizeof(buffer));
    log_line("通道 %u 音频参数：%u Hz / config %u / format %u", channel, param->sampleRate,
             param->channelConfig, param->sampleFormat);
}

static void cb_video_data(u8* data, u32 length)
{
    send_frame(CH_VIDEO, EV_VIDEO_DATA, data, length);
}

static void cb_video_heartbeat(void)
{
    send_frame(CH_VIDEO, EV_VIDEO_HEARTBEAT, NULL, 0);
}

static void cb_media_init(S_AUDIO_INIT_PARAMETER* param)
{
    send_audio_init(CH_AUDIO, param);
}
static void cb_media_data(u8* data, u32 length)
{
    send_frame(CH_AUDIO, EV_AUDIO_DATA, data, length);
}
static void cb_media_stop(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_STOP, NULL, 0);
}
static void cb_media_pause(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_PAUSE, NULL, 0);
}
static void cb_media_resume(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_RESUME, NULL, 0);
}
static void cb_media_seek(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_SEEK, NULL, 0);
}

static void cb_tts_init(S_AUDIO_INIT_PARAMETER* param)
{
    send_audio_init(CH_TTS, param);
}
static void cb_tts_data(u8* data, u32 length)
{
    send_frame(CH_TTS, EV_AUDIO_DATA, data, length);
}
static void cb_tts_stop(void)
{
    send_frame(CH_TTS, EV_AUDIO_STOP, NULL, 0);
}

static void cb_vr_init(S_AUDIO_INIT_PARAMETER* param)
{
    send_audio_init(CH_VR, param);
}
static void cb_vr_data(u8* data, u32 length)
{
    send_frame(CH_VR, EV_AUDIO_DATA, data, length);
}
static void cb_vr_stop(void)
{
    send_frame(CH_VR, EV_AUDIO_STOP, NULL, 0);
}

static void cb_ctrl_ui_action_sound(void)
{
    send_frame(CH_CTRL, EV_CTRL_UI_ACTION_SOUND, NULL, 0);
}

/* ------------------------------------------------------------ 通道接收线程 */

static void* cmd_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->cmdRegisterProtocolVersionMatchStatus(cb_protocol_match);
    lib->cmdRegisterMDInfro(cb_md_info);
    lib->cmdRegisterMDBTPairInfro(cb_md_bt_pair_info);
    lib->cmdRegisterVideoEncoderInitDone(cb_video_encoder_init_done);
    lib->cmdRegisterVideoEncoderFrameRateChangeDone(cb_video_frame_rate_done);
    lib->cmdRegisterTelStateChangeIncoming(cb_tele_incoming);
    lib->cmdRegisterTelStateChangeOutGoing(cb_tele_outgoing);
    lib->cmdRegisterTelStateChangeIdle(cb_tele_idle);
    lib->cmdRegisterTelStateChangeInCalling(cb_tele_in_calling);
    lib->cmdRegisterScreenOn(cb_screen_on);
    lib->cmdRegisterScreenOff(cb_screen_off);
    lib->cmdRegisterScreenUserPresent(cb_screen_user_present);
    lib->cmdRegisterForeground(cb_foreground);
    lib->cmdRegisterBackground(cb_background);
    lib->cmdRegisterGoToDeskTop(cb_go_to_desktop);
    lib->cmdRegisterMicRecordWakeupStart(cb_mic_wakeup);
    lib->cmdRegisterMicRecordEnd(cb_mic_end);
    lib->cmdRegisterMicRecordRecogStart(cb_mic_recog);
    lib->cmdRegisterModuleStatus(cb_module_status);
    lib->cmdRegisterNaviNextTurnInfo(cb_navi_next_turn);
    lib->cmdRegisterFeatureConfigRequest(cb_feature_config_request);
    lib->cmdRegisterMdExit(cb_md_exit);

    while (lib->cmdReceiveOperation() != -1) {
    }
    log_line("命令通道接收线程退出");
    return NULL;
}

static void* video_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->videoRegisterDataReceive(cb_video_data);
    lib->videoRegisterHeartBeat(cb_video_heartbeat);

    while (lib->videoReceiveOperation() != -1) {
    }
    log_line("视频通道接收线程退出");
    return NULL;
}

static void* media_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->mediaRegisterInit(cb_media_init);
    lib->mediaRegisterNormalData(cb_media_data);
    lib->mediaRegisterStop(cb_media_stop);
    lib->mediaRegisterPause(cb_media_pause);
    lib->mediaRegisterResume(cb_media_resume);
    lib->mediaRegisterSeek(cb_media_seek);

    while (lib->mediaReceiveOperation() != -1) {
    }
    log_line("音频通道接收线程退出");
    return NULL;
}

static void* tts_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->ttsRegisterInit(cb_tts_init);
    lib->ttsRegisterNormalData(cb_tts_data);
    lib->ttsRegisterStop(cb_tts_stop);

    while (lib->ttsReceiveOperation() != -1) {
    }
    log_line("导航播报通道接收线程退出");
    return NULL;
}

static void* vr_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->vrRegisterInit(cb_vr_init);
    lib->vrRegisterNormalData(cb_vr_data);
    lib->vrRegisterStop(cb_vr_stop);

    while (lib->vrReceiveOperation() != -1) {
    }
    log_line("语音通道接收线程退出");
    return NULL;
}

static void* ctrl_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->ctrlRegisterUIActionSound(cb_ctrl_ui_action_sound);

    while (lib->ctrlReceiveOperation() != -1) {
    }
    log_line("控制通道接收线程退出");
    return NULL;
}

/* ---------------------------------------------------------------- 命令分发 */

static void handle_command(uint8_t channel, const uint8_t* payload, uint32_t length)
{
    if (length < 1) {
        return;
    }
    uint8_t tag = payload[0];
    const uint8_t* body = payload + 1;
    uint32_t body_length = length - 1;
    CCarLifeLib* lib = CCarLifeLib::getInstance();

    if (channel == CH_CMD) {
        switch (tag) {
        case CMD_PROTOCOL_VERSION: {
            if (body_length < 8) {
                return;
            }
            S_HU_PROTOCOL_VERSION version;
            version.majorVersion = get_be32(body);
            version.minorVersion = get_be32(body + 4);
            lib->cmdHUProtoclVersion(&version);
            log_line("发送协议版本 %u.%u", version.majorVersion, version.minorVersion);
            break;
        }
        case CMD_VIDEO_ENCODER_INIT: {
            if (body_length < 12) {
                return;
            }
            S_VIDEO_ENCODER_INIT param;
            param.width = get_be32(body);
            param.height = get_be32(body + 4);
            param.frameRate = get_be32(body + 8);
            lib->cmdVideoEncoderInit(&param);
            log_line("请求视频编码 %ux%u@%u", param.width, param.height, param.frameRate);
            break;
        }
        case CMD_VIDEO_ENCODER_START:
            lib->cmdVideoEncoderStart();
            log_line("启动视频编码");
            break;
        case CMD_VIDEO_ENCODER_PAUSE:
            lib->cmdVideoEncoderPause();
            log_line("暂停视频编码");
            break;
        case CMD_VIDEO_ENCODER_RESET:
            lib->cmdVideoEncoderReset();
            log_line("复位视频编码");
            break;
        case CMD_GO_TO_FOREGROUND:
            lib->cmdGoToForeground();
            log_line("请求手机切到前台");
            break;
        case CMD_LAUNCH_MODE_NORMAL:
            lib->cmdLaunchModeNormal();
            break;
        case CMD_LAUNCH_MODE_PHONE:
            lib->cmdLaunchModePhone();
            break;
        case CMD_LAUNCH_MODE_MAP:
            lib->cmdLaunchModeMap();
            break;
        case CMD_LAUNCH_MODE_MUSIC:
            lib->cmdLaunchModeMusic();
            break;
        default:
            log_line("未知命令通道指令 tag=%u", tag);
            break;
        }
        return;
    }

    if (channel == CH_CTRL) {
        switch (tag) {
        case CMD_TOUCH_ACTION: {
            if (body_length < 12) {
                return;
            }
            S_TOUCH_ACTION action;
            action.action = get_be32(body);
            action.x = get_be32(body + 4);
            action.y = get_be32(body + 8);
            lib->ctrlTouchAction(&action);
            break;
        }
        case CMD_TOUCH_DOWN: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_DOWN point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchActionDown(&point);
            break;
        }
        case CMD_TOUCH_UP: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_UP point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchActionUp(&point);
            break;
        }
        case CMD_TOUCH_MOVE: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_MOVE point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchActionMove(&point);
            break;
        }
        case CMD_TOUCH_SINGLE_CLICK: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_SIGNAL_CLICK point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchSigleClick(&point);
            break;
        }
        case CMD_TOUCH_DOUBLE_CLICK: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_DOUBLE_CLICK point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchDoubleClick(&point);
            break;
        }
        case CMD_TOUCH_LONG_PRESS: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_LONG_PRESS point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            lib->ctrlTouchLongPress(&point);
            break;
        }
        case CMD_HARD_KEY: {
            if (body_length < 4) {
                return;
            }
            S_TOUCH_CAR_HARD_KEY_CODE key;
            key.keycode = get_be32(body);
            lib->ctrlTouchCarHardKeyCode(&key);
            log_line("发送硬按键 0x%08X", key.keycode);
            break;
        }
        default:
            log_line("未知控制通道指令 tag=%u", tag);
            break;
        }
        return;
    }

    if (channel == CH_VR) {
        if (tag == CMD_VR_DATA) {
            if (body_length < 4) {
                return;
            }
            uint32_t timestamp = get_be32(body);
            lib->sendVRRecordData((u8*)(body + 4), body_length - 4, timestamp);
            return;
        }
        log_line("未知语音通道指令 tag=%u", tag);
    }
}

/* ------------------------------------------------------------ socket 服务 */

static int read_all(int fd, uint8_t* buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t got = ::recv(fd, buffer + offset, length - offset, 0);
        if (got == 0) {
            return 0;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        offset += (size_t)got;
    }
    return 1;
}

static void serve_client(int fd)
{
    uint8_t header[CL_FRAME_HEADER];
    std::vector<uint8_t> payload;
    while (read_all(fd, header, sizeof(header))) {
        uint8_t channel = header[0];
        uint32_t payload_length = get_be32(header + 1);
        if (payload_length == 0 || payload_length > CL_MAX_PAYLOAD) {
            log_line("收到非法帧：channel=%u length=%u", channel, payload_length);
            break;
        }
        payload.resize(payload_length);
        if (!read_all(fd, &payload[0], payload_length)) {
            break;
        }
        handle_command(channel, &payload[0], payload_length);
    }
}

static int listen_unix(const char* path)
{
    ::unlink(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_line("创建 socket 失败：%s", strerror(errno));
        return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (::bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        log_line("绑定 %s 失败：%s", path, strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        log_line("监听失败：%s", strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

static void start_thread(void* (*entry)(void*), const char* name)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, entry, NULL) != 0) {
        log_line("%s 线程创建失败", name);
        return;
    }
    pthread_detach(thread);
}

static void usage(const char* program)
{
    fprintf(stderr,
            "用法：%s [选项]\n"
            "\n"
            "  --socket <路径>   Unix socket 路径（默认 %s）\n"
            "  -q, --quiet       不打印本进程的运行日志\n"
            "  -v, --verbose     连协议栈内部日志一起输出（默认丢弃）\n"
            "  -h, --help        显示本帮助\n"
            "\n"
            "启动顺序：先 tools/usb-bridge，再本进程，最后 server/carlifed。\n",
            program, CL_DEFAULT_SOCKET);
}

int main(int argc, char** argv)
{
    const char* socket_path = CL_DEFAULT_SOCKET;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数：%s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* 协议栈对每条消息都打一行日志，视频帧也不例外，正常运行时会淹掉终端；
     * 除非显式要求，否则把它整个丢掉，只留本进程的诊断输出（走 stderr）。 */
    if (!g_verbose) {
        freopen("/dev/null", "w", stdout);
    }

    /* 对端先断时不要被 SIGPIPE 带走 */
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = listen_unix(socket_path);
    if (g_listen_fd < 0) {
        return 1;
    }
    log_line("监听 %s", socket_path);

    if (!wait_for_bridge(CL_BRIDGE_PORT, 120)) {
        log_line("等待 usb-bridge 超时（127.0.0.1:%d）。请先启动 tools/usb-bridge 再运行本进程。",
                 CL_BRIDGE_PORT);
        return 1;
    }
    log_line("usb-bridge 已就绪");

    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->carLifeLibInit();

    for (int attempt = 1; attempt <= 3; ++attempt) {
        g_connection_result = lib->connectionSetup();
        if (g_connection_result == 0) {
            break;
        }
        log_line("第 %d 次建立通道失败（返回码 %d），1 秒后重试", attempt, g_connection_result);
        sleep(1);
    }
    if (g_connection_result != 0) {
        log_line("无法建立通道，退出。请确认 usb-bridge 正在运行。");
        return 1;
    }
    log_line("六条通道已连接（127.0.0.1 本地端口）");

    start_thread(cmd_receive_thread, "命令通道");
    start_thread(video_receive_thread, "视频通道");
    start_thread(media_receive_thread, "音频通道");
    start_thread(tts_receive_thread, "播报通道");
    start_thread(vr_receive_thread, "语音通道");
    start_thread(ctrl_receive_thread, "控制通道");

    log_line("就绪，等待 Go 后端接入");

    for (;;) {
        struct sockaddr_un peer;
        socklen_t peer_length = sizeof(peer);
        int fd = ::accept(g_listen_fd, (struct sockaddr*)&peer, &peer_length);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_line("accept 失败：%s", strerror(errno));
            break;
        }

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd >= 0) {
            ::close(g_client_fd);
        }
        g_client_fd = fd;
        pthread_mutex_unlock(&g_send_lock);
        log_line("Go 后端已接入");

        broadcast_status();

        serve_client(fd);

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd == fd) {
            g_client_fd = -1;
        }
        pthread_mutex_unlock(&g_send_lock);
        ::close(fd);
        log_line("Go 后端已断开，继续等待重连");
    }

    ::close(g_listen_fd);
    ::unlink(socket_path);
    return 0;
}
