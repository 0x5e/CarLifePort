/*
 * carlife-link -- the phone link: USB transport plus CarLife head-unit protocol stack.
 *
 * Upstream is the phone over USB(AOA): bridge.c owns that side and turns the single USB
 * bulk channel into the six TCP channels CarLifeVehicleLib connects to on 127.0.0.1.
 * Downstream is the Go backend: the data of those six channels is written to an AF_UNIX
 * socket as length-prefixed frames, and the commands received on the same socket are
 * forwarded back to the phone.
 *
 * Process model: bridge_start() claims the phone and opens the six local ports, this file
 * then brings up the library, one receive thread each for cmd/video/audio/tts/vr/ctrl, and
 * a thread that serves the Go backend. The main thread runs the USB receive loop.
 */
#include "CCarLifeLibWrapper.h"
#include "CConnectManager.h"
#include "bridge.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace CCarLifeLibH;

/* ----------------------------------------------------------------- Globals */

static int g_listen_fd = -1;
static int g_client_fd = -1;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Return code of connectionSetup(); 0 means all six channels are established */
static int g_connection_result = -1;

static int g_quiet = 0;
static int g_verbose = 0;

/* Identity reported to the phone during the handshake. The phone authenticates the head
 * unit from the statistics info it sends and checks the vendor channel number Baidu hands
 * out to head unit vendors (guide 17.2); the open source vehicle library ships none, so
 * this is the aftermarket channel its own build defaults to, "20000000"
 * (CommonParams.VEHICLE_CHANNEL_AFTER_MARKET). Override with --channel. See README
 * ("Head unit identity"). */
static std::string g_channel = "20000000";
static std::string g_cuid = "CarLifePort";

/* -------------------------------------------------------- Logging and utils */

/* Shared body of log_line and log_line_debug. A level marker is written right after the
 * timestamp, which is where the Go backend looks for it when it relays these lines: a
 * marked line is dropped unless the backend runs at debug level. The subsystem token after
 * it names this half of the link, which the backend turns back into a module name. */
static void log_vline(const char* level, const char* fmt, va_list args)
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
    if (level != NULL) {
        fprintf(stderr, "%s ", level);
    }
    fputs("stack ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    fflush(stderr);
}

static void log_line(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline(NULL, fmt, args);
    va_end(args);
}

/* For whatever the phone drives on a timer, such as the playback progress bar: worth
 * keeping while a session is being watched live, too repetitive to keep by default. */
static void log_line_debug(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline("DEBUG", fmt, args);
    va_end(args);
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

/* ---------------------------------------------------------- Socket output */

/* Children must never inherit our descriptors. The library runs commands through
 * popen(), and an inherited listener keeps the socket alive after this process exits,
 * which leaves the Go backend connected to a socket nobody accepts on. */
static void set_close_on_exec(int fd)
{
    int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

static int write_all(int fd, const uint8_t* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        // The library headers pull in "using namespace std", so system calls
        // must be invoked with explicit global scope; otherwise names like
        // bind/send collide with same-named standard library functions.
        ssize_t written = ::send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return 0;
        }
        offset += (size_t)written;
    }
    return 1;
}

/* Send one frame to the current client. Partial writes are serialized by the
 * send lock to avoid interleaving across channels. */
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
            log_line("failed to write to client: %s", strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_send_lock);
}

static void send_text(uint8_t tag, const std::string& text)
{
    send_frame(CH_CMD, tag, text.data(), (uint32_t)text.size());
}

/* Report head-unit channel status; sent once when a client connects and once on
 * each status change */
static void broadcast_status(void)
{
    uint8_t status[5];
    status[0] = (g_connection_result == 0) ? 1 : 0;
    put_be32(status + 1, (uint32_t)g_connection_result);
    send_frame(CH_CMD, EV_STATUS, status, sizeof(status));
}

/* --------------------------------------------- Command channel callbacks (phone -> head unit) */

static void cb_protocol_match(S_PROTOCOL_VERSION_MATCH_SATUS* status)
{
    uint8_t buffer[4];
    put_be32(buffer, status->matchStatus);
    send_frame(CH_CMD, EV_PROTOCOL_MATCH, buffer, sizeof(buffer));
    log_line("protocol version match status: %u", status->matchStatus);
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
    log_line("phone device info: %s %s (Android %s)", info->manufacturer.c_str(), info->model.c_str(),
             info->release.c_str());
}

static void cb_md_bt_pair_info(S_BT_PAIR_INFO* info)
{
    log_line("bluetooth pairing info: name=%s status=%u", info->name.c_str(), info->status);
}

static void cb_video_encoder_init_done(S_VIDEO_ENCODER_INIT_DONE* info)
{
    uint8_t buffer[12];
    put_be32(buffer, info->width);
    put_be32(buffer + 4, info->height);
    put_be32(buffer + 8, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_INIT_DONE, buffer, sizeof(buffer));
    log_line("video encoder ready: %ux%u@%u", info->width, info->height, info->frameRate);
}

static void cb_video_frame_rate_done(S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE* info)
{
    uint8_t buffer[4];
    put_be32(buffer, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_FPS_DONE, buffer, sizeof(buffer));
    log_line("frame rate changed: %u", info->frameRate);
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
    log_line("phone screen on");
}

static void cb_screen_off(void)
{
    send_frame(CH_CMD, EV_SCREEN_OFF, NULL, 0);
    log_line("phone screen off");
}

static void cb_screen_user_present(void)
{
    log_line("phone detected user present");
}

static void cb_foreground(void)
{
    send_frame(CH_CMD, EV_FOREGROUND, NULL, 0);
    log_line("CarLife moved to foreground");
}

static void cb_background(void)
{
    send_frame(CH_CMD, EV_BACKGROUND, NULL, 0);
    log_line("CarLife moved to background");
}

static void cb_go_to_desktop(void)
{
    log_line("phone returned to home screen");
}

static void cb_mic_wakeup(void)
{
    send_frame(CH_CMD, EV_MIC_WAKEUP, NULL, 0);
    log_line("voice wakeup");
}

static void cb_mic_end(void)
{
    send_frame(CH_CMD, EV_MIC_END, NULL, 0);
    log_line("voice recording ended");
}

static void cb_mic_recog(void)
{
    send_frame(CH_CMD, EV_MIC_RECOG, NULL, 0);
    log_line("voice recognition started");
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
    log_line("navigation turn prompt: action=%u nextTurn=%u road=%s", info->action, info->nextTurn, info->roadName.c_str());
}

/* ------------------------------------------------------- Head unit identity
 *
 * A real head unit announces itself right after the protocol version and before the
 * video encoder is initialised -- captured traffic shows this order:
 *   head unit --MSG_CMD_STATISTIC_INFO (0x00018027)--> phone
 *   head unit --MSG_CMD_HU_INFO (0x00018003)--> phone
 *
 * The library's bundled test program sends neither.
 *
 * The two messages carry more than statistics: the phone authenticates the head unit from
 * the statistics info it receives (guide 17.2), and only once that check passes does it
 * answer with MD_AUTHEN_RESULT and start the vendor specific services -- which is what
 * makes it ask for the feature configuration (MSG_CMD_MD_FEATURE_CONFIG_REQUEST
 * 0x00010051). With no identity reported at all the phone never asks, and the feature
 * config reply below has nothing to answer. Guide 17.2.2 also warns that a cuid shared by
 * two head units makes the phone drop the connection. */
static unsigned int g_connect_count = 0;

static void send_statistic_info(void)
{
    S_STATISTICS_INFO info;
    info.cuid = g_cuid;
    info.versionName = "1.0.0";
    info.versionCode = 1;
    info.channel = g_channel;
    info.connectCount = ++g_connect_count;
    info.connectSuccessCount = 1; /* the reference head unit always reports 1 (CarlifeUtil) */
    info.connectTime = 0; /* the units of this counter are not documented */
    info.crashLog = "";

    if (CCarLifeLib::getInstance()->cmdStatisticInfo(&info) < 0) {
        log_line("failed to send statistics info");
        return;
    }
    log_line("sent statistics info (channel=%s, cuid=%s)", info.channel.c_str(), info.cuid.c_str());
}

static void send_hu_info(void)
{
    struct utsname uts;
    if (uname(&uts) != 0) {
        log_line("failed to read host identity: %s", strerror(errno));
        return;
    }

    S_HU_INFO info;
    info.os = "Linux";
    info.board = uts.machine;
    info.bootloader = "unknown";
    info.brand = "CarLifePort";
    info.cpu_abi = uts.machine;
    info.cpu_abi2 = "";
    info.device = uts.nodename;
    info.display = "CarLife head unit";
    info.fingerprint = std::string("CarLifePort/") + uts.machine + "/" + uts.nodename;
    info.hardware = uts.machine;
    info.host = uts.nodename;
    info.cid = "";
    info.manufacturer = "CarLifePort";
    info.model = "CarLife head unit";
    info.product = "CarLifePort";
    info.serial = uts.nodename;
    info.codename = "REL";
    info.incremental = "1";
    info.release = uts.release;
    info.sdk = "0";
    info.sdk_int = 0;
    info.token = ""; /* issued by Baidu together with the channel number */
    info.btaddress = "";

    if (CCarLifeLib::getInstance()->cmdHUInfro(&info) < 0) {
        log_line("failed to send head unit info");
        return;
    }
    log_line("sent head unit info (model=%s, host=%s, release=%s)", info.model.c_str(),
             info.host.c_str(), info.release.c_str());
}

/* ------------------------------------------------------- Head unit authentication
 *
 * The phone expects the head unit to run the authentication exchange below; when it
 * never happens, CarLife warns about an unauthenticated head unit and eventually drops
 * the connection:
 *   head unit --MSG_CMD_HU_AUTHEN_REQUEST (randomValue)--> phone
 *   phone     --MSG_CMD_MD_AUTHEN_RESPONSE (encryptValue)--> head unit
 *   head unit --MSG_CMD_HU_AUTHEN_RESULT (authenResult)--> phone
 *   phone     --MSG_CMD_MD_AUTHEN_RESULT (authenResult)--> head unit
 *
 * The open vehicle library ships no Baidu certificate, so the encrypted value cannot be
 * checked. The head unit is the side that reports the verdict, so the exchange is
 * completed with a positive result. */
static uint8_t g_authen_result = 0;

static void send_authen_request(void)
{
    static const char alphabet[] = "0123456789abcdef";
    S_AUTHEN_REQUEST request;
    request.randomValue.assign(16, '0');
    for (size_t i = 0; i < request.randomValue.size(); ++i) {
        request.randomValue[i] = alphabet[rand() % 16];
    }
    if (CCarLifeLib::getInstance()->cmdHuAuthenRequest(&request) < 0) {
        log_line("failed to send head unit authentication request");
        return;
    }
    log_line("sent head unit authentication request");
}

static void cb_md_authen_response(S_AUTHEN_RESPONSE* response)
{
    log_line("phone authentication response received (%u bytes)", (unsigned int)response->encryptValue.size());
    S_HU_AUTHEN_RESULT result;
    result.authenResult = true;
    if (CCarLifeLib::getInstance()->cmdHuAuthenResult(&result) < 0) {
        log_line("failed to send head unit authentication result");
    }
}

static void cb_md_authen_result(S_MD_AUTHEN_RESULT* result)
{
    g_authen_result = result->authenResult ? 1 : 0;
    log_line("phone authentication result: %s", result->authenResult ? "authenticated" : "rejected");
    send_frame(CH_CMD, EV_AUTHEN_RESULT, &g_authen_result, 1);
}

/* Feature configuration the phone asks for once the handshake has passed. The keys and
 * values are the ones the open source head unit reports (CarlifeConfUtil defaults, sent
 * by FeatureConfigManager), so that a difference in phone behaviour cannot be blamed on
 * this message. AUDIO_TRANSMISSION_MODE is the one that matters for audio: 0 asks for the
 * dedicated channel instead of Bluetooth. INPUT_DISABLE is left out on purpose - the guide
 * does not document it, its effect is on the phone only, and touch input works without it.
 *
 * The library serializes the list as a linked list, walking it cnt times, so cnt has to
 * match the chain length. */
static S_FEATURE_CONFIG_LIST g_feature_config_list;
static S_FEATURE_CONFIG g_feature_config[9];

static void cb_feature_config_request(void)
{
    static const struct {
        const char* key;
        uint32_t value;
    } entries[] = {
        {"VOICE_WAKEUP", 1},            /* ENABLE */
        {"BLUETOOTH_AUTO_PAIR", 0},     /* DISABLE */
        {"BLUETOOTH_INTERNAL_UI", 0},   /* DISABLE */
        {"FOCUS_UI", 0},                /* DISABLE */
        {"VOICE_MIC", 0},               /* USE_VEHICLE_MIC */
        {"MEDIA_SAMPLE_RATE", 0},       /* ORIGINAL_SAMPLE_RATE: the phone picks it */
        {"AUDIO_TRANSMISSION_MODE", 0}, /* INDEPENDENT_CHANNEL_MODE, not Bluetooth */
        {"CONTENT_ENCRYPTION", 0},      /* DISABLE */
        {"ENGINE_TYPE", 0},             /* OIL */
    };
    const int count = (int)(sizeof(entries) / sizeof(entries[0]));

    for (int i = 0; i < count; ++i) {
        g_feature_config[i].key = entries[i].key;
        g_feature_config[i].value = entries[i].value;
        g_feature_config[i].pNext = (i + 1 < count) ? &g_feature_config[i + 1] : NULL;
    }
    g_feature_config_list.cnt = (u32)count;
    g_feature_config_list.pFeatureConfig = &g_feature_config[0];
    CCarLifeLib::getInstance()->cmdFeatureConfigResponse(&g_feature_config_list);

    send_frame(CH_CMD, EV_FEATURE_CONFIG_REQUEST, NULL, 0);
    log_line("replied to phone feature config request (%d keys, audio over the dedicated "
             "channel, no content encryption)",
             count);
}

/* What the phone's media player is doing, reported on the command channel: the track
 * whenever it changes and the position once a second while it plays. Neither drives
 * anything, but they are the only way to tell whether the phone's music module is really
 * running - which is what decides whether the audio channel carries anything at all. */
static void cb_media_info(S_MEDIA_INFO* info)
{
    log_line("media info: %s - %s [%s] %u s", info->song.c_str(), info->artist.c_str(),
             info->source.c_str(), info->duration);
}

static void cb_media_progress_bar(S_MEDIA_PROGRESS_BAR* progress)
{
    log_line_debug("media progress: %u ms", progress->progressBar);
}

static void cb_connect_exception(S_CONNECTION_EXCEPTION* exception)
{
    log_line("phone reported a connection exception, type %u", exception->exceptionType);
}

static void cb_md_exit(void)
{
    send_frame(CH_CMD, EV_MD_EXIT, NULL, 0);
    log_line("CarLife exited on phone");
}

/* ------------------------------------------------------- Audio/video channels */

static void send_audio_init(uint8_t channel, S_AUDIO_INIT_PARAMETER* param)
{
    uint8_t buffer[12];
    put_be32(buffer, param->sampleRate);
    put_be32(buffer + 4, param->channelConfig);
    put_be32(buffer + 8, param->sampleFormat);
    send_frame(channel, EV_AUDIO_INIT, buffer, sizeof(buffer));
    log_line("channel %u audio params: %u Hz / config %u / format %u", channel, param->sampleRate,
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

/* ------------------------------------------------- Channel receive threads */

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
    lib->cmdRegisterMediaInfo(cb_media_info);
    lib->cmdRegisterMediaProgressBar(cb_media_progress_bar);
    lib->cmdRegisterConnectException(cb_connect_exception);
    lib->cmdRegisterMdExit(cb_md_exit);
    lib->cmdRegisterMdAuthenResponse(cb_md_authen_response);
    lib->cmdRegisterMdAuthenResult(cb_md_authen_result);

    while (lib->cmdReceiveOperation() != -1) {
    }
    log_line("command channel receive thread exiting");
    return NULL;
}

static void* video_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->videoRegisterDataReceive(cb_video_data);
    lib->videoRegisterHeartBeat(cb_video_heartbeat);

    while (lib->videoReceiveOperation() != -1) {
    }
    log_line("video channel receive thread exiting");
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
    log_line("audio channel receive thread exiting");
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
    log_line("navigation TTS channel receive thread exiting");
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
    log_line("voice channel receive thread exiting");
    return NULL;
}

static void* ctrl_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->ctrlRegisterUIActionSound(cb_ctrl_ui_action_sound);

    while (lib->ctrlReceiveOperation() != -1) {
    }
    log_line("control channel receive thread exiting");
    return NULL;
}

/* --------------------------------------------------------- Command dispatch */

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
            log_line("sending protocol version %u.%u", version.majorVersion, version.minorVersion);

            /* Identity first: the phone authenticates the head unit from it and only
             * then asks for the feature configuration */
            send_statistic_info();
            send_hu_info();
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
            log_line("requesting video encoding %ux%u@%u", param.width, param.height, param.frameRate);
            break;
        }
        case CMD_VIDEO_ENCODER_START:
            lib->cmdVideoEncoderStart();
            log_line("starting video encoder");
            break;
        case CMD_VIDEO_ENCODER_PAUSE:
            lib->cmdVideoEncoderPause();
            log_line("pausing video encoder");
            break;
        case CMD_VIDEO_ENCODER_RESET:
            lib->cmdVideoEncoderReset();
            log_line("resetting video encoder");
            break;
        case CMD_GO_TO_FOREGROUND:
            lib->cmdGoToForeground();
            log_line("requesting phone to move to foreground");
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
        case CMD_AUTHEN_REQUEST:
            if (g_authen_result == 1) {
                log_line("head unit already authenticated, skipping authentication request");
                break;
            }
            send_authen_request();
            break;
        default:
            log_line("unknown command channel tag=%u", tag);
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
            log_line("sending hard key 0x%08X", key.keycode);
            break;
        }
        default:
            log_line("unknown control channel tag=%u", tag);
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
        log_line("unknown voice channel tag=%u", tag);
    }
}

/* ------------------------------------------------------------ Socket server */

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
            log_line("invalid frame received: channel=%u length=%u", channel, payload_length);
            break;
        }
        payload.resize(payload_length);
        if (!read_all(fd, &payload[0], payload_length)) {
            break;
        }
        handle_command(channel, &payload[0], payload_length);
    }
}

/* Serve the Go backend. This runs on its own thread: the main thread owns the USB receive
 * loop, and one backend connection at a time is all carlifed makes. */
static void* backend_loop(void*)
{
    for (;;) {
        struct sockaddr_un peer;
        socklen_t peer_length = sizeof(peer);
        int fd = ::accept(g_listen_fd, (struct sockaddr*)&peer, &peer_length);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_line("accept failed: %s", strerror(errno));
            return NULL;
        }
        set_close_on_exec(fd);

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd >= 0) {
            ::close(g_client_fd);
        }
        g_client_fd = fd;
        pthread_mutex_unlock(&g_send_lock);
        log_line("Go backend connected");

        broadcast_status();

        serve_client(fd);

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd == fd) {
            g_client_fd = -1;
        }
        pthread_mutex_unlock(&g_send_lock);
        ::close(fd);
        log_line("Go backend disconnected, waiting for reconnect");
    }
}

static int listen_unix(const char* path)
{
    ::unlink(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_line("failed to create socket: %s", strerror(errno));
        return -1;
    }
    set_close_on_exec(fd);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (::bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        log_line("failed to bind %s: %s", path, strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        log_line("failed to listen: %s", strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

static void start_thread(void* (*entry)(void*), const char* name)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, entry, NULL) != 0) {
        log_line("failed to create %s thread", name);
        return;
    }
    pthread_detach(thread);
}

/* Open the six channel sockets towards the local ports bridge.c listens on.
 *
 * This is what CCarLifeLib::connectionSetup() does minus its first step, which runs six
 * `adb forward` commands. Skipping that step is deliberate:
 *   1. This head unit does not use adb -- the bridge owns the USB link and listens on the
 *      127.0.0.1 ports below. Running `adb forward` restarts the adb server, and a running
 *      adb server holds the phone, so the bridge's next USB claim fails with
 *      LIBUSB_ERROR_ACCESS (this is the `adb kill-server` requirement, reintroduced by the
 *      library itself).
 *   2. popen() gives the adb child every descriptor this process has open, the listening
 *      socket included.
 * Nothing else in connectionSetup() is lost: it only forwards and then calls these six
 * create methods. */
static int setup_channels(void)
{
    CConnectManager* manager = CConnectManager::getInstance();
    if (!manager->createCmdSocket()) {
        return -1;
    }
    if (!manager->createVideoSocket()) {
        return -1;
    }
    if (!manager->createMediaSocket()) {
        return -1;
    }
    if (!manager->createTTSSocket()) {
        return -1;
    }
    if (!manager->createVRSocket()) {
        return -1;
    }
    if (!manager->createTouchSocket()) {
        return -1;
    }
    return 0;
}

static void usage(const char* program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "  --socket <path>    Unix socket the Go backend connects to (default %s)\n"
            "  --channel <name>   vendor channel number reported to the phone (default 20000000)\n"
            "  --cuid <id>        head unit id reported to the phone (default CarLifePort)\n"
            "  --host <address>   local address of the six channel ports (default 127.0.0.1)\n"
            "  --vid <0xXXXX>     target device VID (default: pick a phone)\n"
            "  --pid <0xXXXX>     target device PID (default: pick a phone)\n"
            "  --force-handshake  resend 51/52/53 even if the phone is already in accessory mode\n"
            "  --list             list the USB devices and exit, without touching any device\n"
            "  --usb-only         run the USB transport without the protocol stack\n"
            "  -q, --quiet        do not print this process's runtime logs\n"
            "  -v, --verbose      also emit the protocol stack's internal logs (discarded by default)\n"
            "  -h, --help         show this help\n",
            program, CL_DEFAULT_SOCKET);
}

int main(int argc, char** argv)
{
    const char* socket_path = CL_DEFAULT_SOCKET;
    const char* channel_host = "127.0.0.1";
    int vid = -1;
    int pid = -1;
    int force_handshake = 0;
    int list_only = 0;
    int usb_only = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            g_channel = argv[++i];
        } else if (strcmp(argv[i], "--cuid") == 0 && i + 1 < argc) {
            g_cuid = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            channel_host = argv[++i];
        } else if (strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            vid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--force-handshake") == 0) {
            force_handshake = 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--usb-only") == 0) {
            usb_only = 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* The protocol stack logs one line per message, video frames included,
     * which floods the terminal in normal operation; unless explicitly
     * requested, discard it entirely and keep only this process's diagnostics
     * (on stderr, which is also where the transport writes). */
    if (!g_verbose) {
        freopen("/dev/null", "w", stdout);
    }

    /* Seed the random value of the authentication request */
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    if (list_only) {
        return bridge_list_devices();
    }

    /* The transport half comes first: its six ports have to exist before the library
     * connects, and it waits here until a phone is plugged in. bridge_start() also
     * installs the SIGINT/SIGTERM handler that stops both halves. */
    if (bridge_start(channel_host, vid, pid, force_handshake, g_quiet) != 0) {
        return 1;
    }
    if (usb_only) {
        int rc = bridge_loop();
        bridge_shutdown();
        return rc == 0 ? 0 : 1;
    }

    g_listen_fd = listen_unix(socket_path);
    if (g_listen_fd < 0) {
        bridge_shutdown();
        return 1;
    }
    log_line("listening on %s", socket_path);

    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->carLifeLibInit();

    for (int attempt = 1; attempt <= 3; ++attempt) {
        g_connection_result = setup_channels();
        if (g_connection_result == 0) {
            break;
        }
        log_line("channel setup attempt %d failed (return code %d), retrying in 1 second", attempt, g_connection_result);
        sleep(1);
    }
    if (g_connection_result != 0) {
        log_line("failed to establish the six channels, exiting");
        ::close(g_listen_fd);
        ::unlink(socket_path);
        bridge_shutdown();
        return 1;
    }
    log_line("all six channels connected (127.0.0.1 local ports)");

    start_thread(cmd_receive_thread, "command channel");
    start_thread(video_receive_thread, "video channel");
    start_thread(media_receive_thread, "audio channel");
    start_thread(tts_receive_thread, "TTS channel");
    start_thread(vr_receive_thread, "voice channel");
    start_thread(ctrl_receive_thread, "control channel");
    start_thread(backend_loop, "backend");

    log_line("ready, waiting for the Go backend to connect");

    /* The USB receive loop owns the main thread: it returns when the phone goes away, and
     * the process ends there so that carlifed rebuilds the link. */
    int rc = bridge_loop();

    bridge_shutdown();
    ::close(g_listen_fd);
    ::unlink(socket_path);
    return rc == 0 ? 0 : 1;
}
