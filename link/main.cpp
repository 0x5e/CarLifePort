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
#include "gps.h"
#include "log.h"
#include "md_authen.h"
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

#include <atomic>
#include <string>
#include <vector>

using namespace CCarLifeLibH;

/* ----------------------------------------------------------------- Globals */

static int g_listen_fd = -1;
static int g_client_fd = -1;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Serializes every message the library writes on the command channel. The library builds
 * a message as an 8-byte header plus a body and hands them to two separate send() calls
 * without a lock (CConnectManager::writeCmdData), so two threads writing at the same time
 * leave a header and another message's body next to each other. The backend thread writes
 * commands from the Go side, the command channel thread writes the replies, and the
 * vehicle data reporter (see below) adds a third writer that runs on a timer. */
static pthread_mutex_t g_cmd_write_lock = PTHREAD_MUTEX_INITIALIZER;

/* Scope guard for g_cmd_write_lock, held across one complete library call. */
struct CmdWriteLock {
    CmdWriteLock() { pthread_mutex_lock(&g_cmd_write_lock); }
    ~CmdWriteLock() { pthread_mutex_unlock(&g_cmd_write_lock); }
};

/* Return code of connectionSetup(); 0 means all six channels are established */
static int g_connection_result = -1;

static int g_quiet = 0;
static int g_verbose = 0;

/* Identity reported to the phone during the handshake. The phone authenticates the head
 * unit from the statistics info it sends and checks the vendor channel number Baidu hands
 * out to head unit vendors (guide 17.2). Both are supplied by --channel and --cuid: the
 * Go backend owns the defaults, so this process has none of its own. See README ("Head
 * unit identity"). */
static std::string g_channel;
static std::string g_cuid;

/* -------------------------------------------------------- Logging and utils */

/* Log line format
 *
 * Every line is "<event> <key>=<value> ...". The event is a stable, lower-case, dotted name
 * of the form <area>.<subject>_<outcome> (protocol_version_sent, read_failed, frame_rejected)
 * and is what a search keys on, so it outlives the fields around it. Fields carry the
 * numbers, unit in the key (retry_ms, rate_hz, acc_m) and never in the value. Prose belongs
 * in neither half: the level says how bad it is, the event says what happened.
 */

/* Shared body of the log_line* functions. The level marker is written right after the
 * timestamp, which is where the Go backend looks for it when it relays these lines: it
 * becomes the line's level in the backend's own level column, and a line marked DEBUG is
 * dropped unless the backend runs at debug level. No marker means info. The subsystem token
 * after it names this half of the link, which the backend turns back into a module name. */
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

    /* One line at a time: the six channel threads, the USB loop and the backend thread all
     * log here, and the line is written piece by piece. */
    aoa_log_lock();
    fprintf(stderr, "[%02d:%02d:%02d.%03d] ", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
            (int)(tv.tv_usec / 1000));
    if (level != NULL) {
        fprintf(stderr, "%s ", level);
    }
    fputs("stack ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    fflush(stderr);
    aoa_log_unlock();
}

/* The format attribute is what makes the compiler check the arguments against the format
 * string: without it a mismatched %u or a missing argument compiles silently and only shows
 * up as a wrong number in the log. */
__attribute__((format(printf, 1, 2))) static void log_line(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline(NULL, fmt, args);
    va_end(args);
}

/* For lines that would otherwise repeat what the Go backend already reports: everything
 * this half forwards as a frame and the backend logs from it, plus whatever the phone
 * drives on a timer, such as the playback progress bar. Both are worth keeping while the
 * protocol is being worked on, too repetitive to keep by default. */
__attribute__((format(printf, 1, 2))) static void log_line_debug(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline("DEBUG", fmt, args);
    va_end(args);
}

/* Something did not work but the link keeps running: a step of the bring-up loop that will
 * be retried, a frame or a connection that had to be dropped, an event this half does not
 * know. Anything a failure leaves behind on the way out is an error instead. */
__attribute__((format(printf, 1, 2))) static void log_line_warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline("WARN", fmt, args);
    va_end(args);
}

/* A failure: the operation was abandoned, or the link cannot continue and is about to be
 * rebuilt by the backend. */
__attribute__((format(printf, 1, 2))) static void log_line_error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_vline("ERROR", fmt, args);
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
            log_line_error("link.client_write_failed err=%s", strerror(errno));
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
}

static void cb_md_bt_pair_info(S_BT_PAIR_INFO* info)
{
    log_line("phone.bluetooth_pairing name=\"%s\" status=%u", info->name.c_str(), info->status);
}

static void cb_video_encoder_init_done(S_VIDEO_ENCODER_INIT_DONE* info)
{
    uint8_t buffer[12];
    put_be32(buffer, info->width);
    put_be32(buffer + 4, info->height);
    put_be32(buffer + 8, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_INIT_DONE, buffer, sizeof(buffer));
    log_line_debug("video.encoder_ready width=%u height=%u fps=%u", info->width, info->height,
                   info->frameRate);
}

static void cb_video_frame_rate_done(S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE* info)
{
    uint8_t buffer[4];
    put_be32(buffer, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_FPS_DONE, buffer, sizeof(buffer));
    log_line("video.frame_rate_changed fps=%u", info->frameRate);
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
    log_line_debug("phone.screen state=on");
}

static void cb_screen_off(void)
{
    send_frame(CH_CMD, EV_SCREEN_OFF, NULL, 0);
    log_line_debug("phone.screen state=off");
}

static void cb_screen_user_present(void)
{
    log_line("phone.user_present");
}

static void cb_foreground(void)
{
    send_frame(CH_CMD, EV_FOREGROUND, NULL, 0);
    log_line_debug("phone.app_state state=foreground");
}

static void cb_background(void)
{
    send_frame(CH_CMD, EV_BACKGROUND, NULL, 0);
    log_line_debug("phone.app_state state=background");
}

static void cb_go_to_desktop(void)
{
    send_frame(CH_CMD, EV_GO_TO_DESKTOP, NULL, 0);
    log_line_debug("phone.app_state state=home");
}

static void cb_mic_wakeup(void)
{
    send_frame(CH_CMD, EV_MIC_WAKEUP, NULL, 0);
    log_line_debug("voice.wakeup");
}

static void cb_mic_end(void)
{
    send_frame(CH_CMD, EV_MIC_END, NULL, 0);
    log_line_debug("voice.record_stopped");
}

static void cb_mic_recog(void)
{
    send_frame(CH_CMD, EV_MIC_RECOG, NULL, 0);
    log_line_debug("voice.recognition_started");
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
    log_line("navi.turn_prompt action=%u next_turn=%u road=\"%s\"", info->action, info->nextTurn,
             info->roadName.c_str());
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

    CmdWriteLock lock;
    if (CCarLifeLib::getInstance()->cmdStatisticInfo(&info) < 0) {
        log_line_error("session.statistic_info_send_failed");
        return;
    }
    log_line("session.statistic_info_sent channel=%s cuid=%s", info.channel.c_str(),
             info.cuid.c_str());
}

static void send_hu_info(void)
{
    struct utsname uts;
    if (uname(&uts) != 0) {
        log_line_error("session.hu_info_failed step=read_identity err=%s", strerror(errno));
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

    CmdWriteLock lock;
    if (CCarLifeLib::getInstance()->cmdHUInfro(&info) < 0) {
        log_line_error("session.hu_info_send_failed");
        return;
    }
    log_line("session.hu_info_sent model=%s host=%s release=%s", info.model.c_str(),
             info.host.c_str(), info.release.c_str());
}

/* ------------------------------------------------------- Head unit authentication
 *
 * Two exchanges back to back, one in each direction (guide 17.1 and 17.2):
 *
 *   head unit --MSG_CMD_HU_AUTHEN_REQUEST (randomValue)--> phone
 *   phone     --MSG_CMD_MD_AUTHEN_RESPONSE (encryptValue)--> head unit
 *   head unit --MSG_CMD_HU_AUTHEN_RESULT (authenResult)--> phone
 *   phone     --MSG_CMD_MD_AUTHEN_RESULT (authenResult)--> head unit
 *
 * The first direction is the one that identifies the phone: it signs the value it is sent
 * with the RSA private key built into the app, and md_authen.c checks that signature with
 * the matching public key. The second direction is the phone's verdict on this head unit,
 * which it reaches from the channel number in the statistics info (see
 * send_statistic_info). The two results are independent and each is reported as it comes
 * out; this side does not treat a rejection as fatal (the guide has the head unit drop the
 * connection when its own check fails, which would make a mistake in the check above look
 * like a phone that cannot be talked to at all).
 *
 * The value that is sent has to carry a version. The phone splits it at the '_' and
 * compares that part with its own version, segment by segment, before deciding what to
 * sign; a value without one makes it throw internally and it never answers. It signs the
 * value unchanged when it is asked for a version at or below its own, which is what the
 * constant below does against an 8.9.0 app (the app itself substitutes 1.1.0 for its own
 * version in a debug build). Raise it only up to the version that is actually installed. */
static const char kAuthenVersion[] = "1.1.0";

/* The value of the outstanding request, kept to check the answer against. It is written on
 * the backend's thread (the command that starts the exchange) and read on the command
 * channel's (the answer), hence the lock. */
static pthread_mutex_t g_authen_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_authen_random;

/* Whether the phone accepted this head unit, as reported by the phone itself */
static uint8_t g_authen_result = 0;

static void send_authen_request(void)
{
    static const char alphabet[] = "0123456789abcdef";
    S_AUTHEN_REQUEST request;
    request.randomValue.assign(16, '0');
    for (size_t i = 0; i < request.randomValue.size(); ++i) {
        request.randomValue[i] = alphabet[rand() % 16];
    }
    request.randomValue += "_";
    request.randomValue += kAuthenVersion;

    pthread_mutex_lock(&g_authen_lock);
    g_authen_random = request.randomValue;
    pthread_mutex_unlock(&g_authen_lock);

    CmdWriteLock lock;
    if (CCarLifeLib::getInstance()->cmdHuAuthenRequest(&request) < 0) {
        log_line_error("auth.request_send_failed");
        return;
    }
    log_line("auth.request_sent value=%s", request.randomValue.c_str());
}

static void cb_md_authen_response(S_AUTHEN_RESPONSE* response)
{
    pthread_mutex_lock(&g_authen_lock);
    std::string expected = g_authen_random;
    pthread_mutex_unlock(&g_authen_lock);

    char recovered[MD_AUTHEN_MAX_MESSAGE + 1];
    int length = md_authen_recover(response->encryptValue.c_str(), recovered, sizeof(recovered));
    bool verified = !expected.empty() && length >= 0 && expected.compare(recovered) == 0;

    if (verified) {
        log_line("auth.response_verified bytes=%d", length);
    } else if (expected.empty()) {
        log_line_warn("auth.response_unsolicited bytes=%u",
                      (unsigned int)response->encryptValue.size());
    } else if (length < 0) {
        log_line_warn("auth.response_invalid bytes=%u reason=not_rsa1024_pkcs1",
                      (unsigned int)response->encryptValue.size());
    } else {
        /* The phone rewrites the version part of the value when it is asked for a version
         * above its own, so a mismatch here usually means kAuthenVersion is too high. */
        log_line_warn("auth.response_mismatch got=\"%s\" want=\"%s\"", recovered,
                      expected.c_str());
    }

    S_HU_AUTHEN_RESULT result;
    result.authenResult = verified;
    CmdWriteLock lock;
    if (CCarLifeLib::getInstance()->cmdHuAuthenResult(&result) < 0) {
        log_line_error("auth.result_send_failed");
    }
}

static void cb_md_authen_result(S_MD_AUTHEN_RESULT* result)
{
    g_authen_result = result->authenResult ? 1 : 0;
    log_line_debug("auth.result_received result=%s",
                   result->authenResult ? "authenticated" : "rejected");
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
    CmdWriteLock lock;
    CCarLifeLib::getInstance()->cmdFeatureConfigResponse(&g_feature_config_list);

    send_frame(CH_CMD, EV_FEATURE_CONFIG_REQUEST, NULL, 0);
    log_line_debug("session.feature_config_replied keys=%d audio=dedicated_channel encryption=none",
             count);
}

/* Names for the play modes, CarlifeMediaPlayMode in guide 13.2 (table 13.2-2). The
 * identifiers are the ones the protocol itself uses, so a log line can be matched against
 * the guide. */
static const char* media_play_mode_name(uint32_t mode)
{
    switch (mode) {
    case 0: return "PLAYMODE_REPEATE_ONE";
    case 1: return "PLAYMODE_SHUFFLE";
    case 2: return "PLAYMODE_REPEATE_ALL";
    default: return "unknown";
    }
}

/* What the phone's media player is doing, reported on the command channel: the track
 * whenever it changes and the position once a second while it plays. Neither drives
 * anything, but they are the only way to tell whether the phone's music module is really
 * running - which is what decides whether the audio channel carries anything at all.
 *
 * All fields of MSG_CMD_MEDIA_INFO (guide 13.2) are logged, under the names the structure
 * uses, as key=value: strings are quoted so that a title with spaces stays readable.
 * albumArt is a bitmap, so only its size is printed, in kB. */
static void cb_media_info(S_MEDIA_INFO* info)
{
    log_line_debug("media.info source=\"%s\" song=\"%s\" artist=\"%s\" album=\"%s\" art_kb=%.1f "
             "duration_s=%u playlist=%u song_id=\"%s\" mode=%s mode_id=%u",
             info->source.c_str(), info->song.c_str(), info->artist.c_str(), info->album.c_str(),
             (double)info->albumArt.size() / 1024.0, info->duration, info->playlistNum,
             info->songId.c_str(), media_play_mode_name(info->mode), info->mode);
}

static void cb_media_progress_bar(S_MEDIA_PROGRESS_BAR* progress)
{
    log_line_debug("media.progress pos_ms=%u", progress->progressBar);
}

static void cb_connect_exception(S_CONNECTION_EXCEPTION* exception)
{
    log_line_warn("phone.connection_exception type=%u", exception->exceptionType);
}

static void cb_md_exit(void)
{
    send_frame(CH_CMD, EV_MD_EXIT, NULL, 0);
    log_line_debug("phone.app_exited");
}

/* ------------------------------------------------------- Audio/video channels */

static void send_audio_init(uint8_t channel, S_AUDIO_INIT_PARAMETER* param)
{
    uint8_t buffer[12];
    put_be32(buffer, param->sampleRate);
    put_be32(buffer + 4, param->channelConfig);
    put_be32(buffer + 8, param->sampleFormat);
    send_frame(channel, EV_AUDIO_INIT, buffer, sizeof(buffer));
    log_line_debug("audio.channel_ready ch=%u rate_hz=%u channel_config=%u sample_format=%u",
             channel, param->sampleRate, param->channelConfig, param->sampleFormat);
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

/* ------------------------------------------------------ Vehicle data: position
 *
 * The phone asks which vehicle data modules this head unit can produce (guide 12):
 * MSG_CMD_CAR_DATA_SUBSCRIBE carries a list, the head unit answers with ..._SUBSCRIBE_DONE
 * carrying the module it supports, the coordinate format and the rate, and the phone then
 * subscribes to one module at a time with ..._START. Only GPS is implemented here, so the
 * reply has a single entry.
 *
 * The request is not a filter: the phone sends it with an empty list (cnt 0) and expects
 * the head unit to report what it has, which is what the reference head unit does - it
 * answers every ..._SUBSCRIBE with its whole list, built from the reporters it created.
 *
 * Of the twenty-five fields MSG_CMD_CAR_GPS defines, only the ones describing where this
 * machine is can be filled: position, altitude, speed, heading, the accuracy of the fix and
 * the time of the fix, all from CoreLocation (gps.mm). Everything else - signal strength,
 * satellite counts, the DOPs - has no source here and is reported as zero, which is what the
 * reference head unit does with the fields it cannot measure either. The message is not
 * skipped for them: every field except timeStamp is required, so a body with any of them
 * missing does not parse on the phone.
 */
static const u32 kCarDataGps = 0;

/* Set by ..._START, cleared by ..._STOP, read by the reporting thread below. */
static std::atomic<int> g_gps_reporting(0);

/* One fix per second, the rate the guide gives for this message. The phone asks for a rate
 * of its own in ..._START and it is not honoured: the fix comes from Wi-Fi positioning, so
 * sampling it faster than the protocol's default would only repeat the same position. */
static const int kGpsReportIntervalSeconds = 1;

/* The library fills cnt entries of a fixed array of ten and leaves the rest as it found
 * it, so only the first cnt entries of a list mean anything. Those ids as "0" or "0,1",
 * "none" when there are none. */
static void format_module_list(const S_VEHICLE_INFO_LIST* list, char* ids, size_t size)
{
    size_t offset = 0;
    ids[0] = '\0';

    unsigned i = 0;
    for (const S_VEHICLE_INFO* info = list->pVehicleInfo;
         info != NULL && i < (unsigned)list->cnt; info = info->pNext, ++i) {
        int written = snprintf(ids + offset, size - offset, offset == 0 ? "%d" : ",%d",
                               (int)info->moduleID);
        if (written > 0 && (size_t)written < size - offset) {
            offset += (size_t)written;
        }
    }
    if (offset == 0) {
        snprintf(ids, size, "none");
    }
}

/* An empty list is read as naming every module. The phone asks for the module list with an
 * empty ..._SUBSCRIBE and takes what the reply offers; an empty ..._START is the same
 * question one step later, so it starts everything this head unit declared. */
static bool vehicle_list_names(const S_VEHICLE_INFO_LIST* list, u32 module)
{
    if (list->cnt == 0) {
        return true;
    }
    unsigned i = 0;
    for (const S_VEHICLE_INFO* info = list->pVehicleInfo;
         info != NULL && i < (unsigned)list->cnt; info = info->pNext, ++i) {
        if (info->moduleID == module) {
            return true;
        }
    }
    return false;
}

static const char* authorization_state(int value)
{
    if (value == 1) {
        return "authorized";
    }
    if (value == -1) {
        return "pending";
    }
    return "denied";
}

static void cb_car_data_subscribe(S_VEHICLE_INFO_LIST* list)
{
    /* Support is declared whether or not a fix is available yet. The phone sends this once
     * per session, a second or two after the link starts, when the Location Services
     * prompt may still be open; answering "no GPS" here would leave the session without
     * position data even after the permission is granted. A position that is not there is
     * reported in the message itself, as antennaState -1 (gps_send_thread), which is the
     * guide's "GPS module unavailable". */
    S_VEHICLE_INFO gps_info;
    gps_info.moduleID = kCarDataGps;
    gps_info.flag = 1; /* WGS84, and "supported" */
    gps_info.frequency = (i32)kGpsReportIntervalSeconds;
    gps_info.pNext = NULL;
    S_VEHICLE_INFO_LIST reply;
    reply.cnt = 1;
    reply.pVehicleInfo = &gps_info;

    {
        CmdWriteLock lock;
        if (CCarLifeLib::getInstance()->cmdCarDataSubscribeDone(&reply) < 0) {
            log_line_error("vehicle.subscribe_reply_failed");
            return;
        }
    }
    char ids[48];
    format_module_list(list, ids, sizeof(ids));
    log_line("vehicle.subscribe_requested modules=%s reply=gps rate_hz=%d auth=%s", ids,
             kGpsReportIntervalSeconds, authorization_state(gps_authorization()));
}

static void cb_car_data_subscribe_start(S_VEHICLE_INFO_LIST* list)
{
    char ids[48];
    format_module_list(list, ids, sizeof(ids));
    if (!vehicle_list_names(list, kCarDataGps)) {
        log_line_warn("vehicle.gps_report_skipped modules=%s reason=not_gps", ids);
        return;
    }
    g_gps_reporting.store(1);
    log_line("vehicle.gps_report_started modules=%s rate_hz=%d", ids, kGpsReportIntervalSeconds);
}

static void cb_car_data_subscribe_stop(S_VEHICLE_INFO_LIST* list)
{
    if (!vehicle_list_names(list, kCarDataGps)) {
        return;
    }
    char ids[48];
    format_module_list(list, ids, sizeof(ids));
    g_gps_reporting.store(0);
    log_line("vehicle.gps_report_stopped modules=%s", ids);
}

/* Reports the latest fix once per interval while the phone is subscribed. Without a fix -
 * because the location service is denied, still unanswered, or simply has none yet - the
 * message is still sent, with antennaState -1, the guide's "GPS module unavailable": the
 * phone then sees a module that exists but has nothing to report, instead of a subscribed
 * module that never sends anything. Logged once per transition, not once per second. */
static void* gps_send_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    int last_authorization = 2; /* no such value, so the first pass always reports */
    bool no_fix_logged = false;
    bool fix_logged = false;
    bool failure_logged = false;

    for (;;) {
        sleep(kGpsReportIntervalSeconds);

        const int authorization = gps_authorization();
        if (authorization != last_authorization) {
            if (authorization == 1) {
                log_line("gps.authorization state=authorized");
            } else {
                log_line_warn("gps.authorization state=%s hint=\"%s\"",
                              authorization_state(authorization),
                              authorization == -1
                                  ? "the prompt for CarLifeLink.app is open; a granted "
                                    "permission is kept across rebuilds"
                                  : "enable Location Services for CarLifeLink.app");
            }
            last_authorization = authorization;
        }

        if (g_gps_reporting.load() == 0) {
            no_fix_logged = false;
            fix_logged = false;
            continue;
        }

        S_CAR_GPS gps;
        memset(&gps, 0, sizeof(gps));
        gps_fix fix;
        if (gps_latest(&fix)) {
            gps.antennaState = 1;
            gps.latitude = (u32)fix.latitude;
            gps.longitude = (u32)fix.longitude;
            gps.height = (u32)fix.height;
            gps.speed = fix.speed;
            gps.heading = fix.heading;
            gps.horPosError = fix.hor_pos_error;
            gps.vertPosError = fix.vert_pos_error;
            gps.timeStamp = fix.timestamp;
            gps.year = fix.year;
            gps.month = fix.month;
            gps.day = fix.day;
            gps.hrs = fix.hour;
            gps.min = fix.minute;
            gps.sec = fix.second;
            if (!fix_logged) {
                log_line("gps.fix lat=%.6f lon=%.6f alt_m=%.0f acc_m=%u "
                         "utc=%04u-%02u-%02uT%02u:%02u:%02uZ",
                         fix.latitude / 1000000.0, fix.longitude / 1000000.0,
                         fix.height / 10.0, fix.hor_pos_error, (unsigned)fix.year,
                         (unsigned)fix.month, (unsigned)fix.day, (unsigned)fix.hour,
                         (unsigned)fix.minute, (unsigned)fix.second);
                fix_logged = true;
            }
            no_fix_logged = false;
        } else {
            gps.antennaState = 0xFFFFFFFFu; /* -1: no fix */
            if (!no_fix_logged) {
                log_line_warn("gps.fix_missing antenna=unavailable reason=%s",
                              authorization == 1 ? "waiting_for_first_fix"
                                                 : authorization_state(authorization));
                no_fix_logged = true;
            }
            fix_logged = false;
        }

        CmdWriteLock lock;
        if (lib->cmdCarGPS(&gps) < 0) {
            if (!failure_logged) {
                log_line_error("gps.send_failed");
                failure_logged = true;
            }
        } else {
            failure_logged = false;
        }
    }
    return NULL;
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
    lib->cmdRegisterCarDataSubscribe(cb_car_data_subscribe);
    lib->cmdRegisterCarDataSubscribeStart(cb_car_data_subscribe_start);
    lib->cmdRegisterCarDataSubscribeStop(cb_car_data_subscribe_stop);

    while (lib->cmdReceiveOperation() != -1) {
    }
    log_line("channel.stopped ch=cmd");
    return NULL;
}

static void* video_receive_thread(void*)
{
    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->videoRegisterDataReceive(cb_video_data);
    lib->videoRegisterHeartBeat(cb_video_heartbeat);

    while (lib->videoReceiveOperation() != -1) {
    }
    log_line("channel.stopped ch=video");
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
    log_line("channel.stopped ch=media");
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
    log_line("channel.stopped ch=tts");
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
    log_line("channel.stopped ch=vr");
    return NULL;
}

/* --------------------------------------------------------- Command dispatch */

/* Names for the hard key codes, table 10.6-1 of the protocol guide. They are the identifiers
   the protocol implementation uses for these codes (KEYCODE_* in the vehicle library and in
   CommonParams.java on the phone side), so a log line can be matched against the vendor
   sources. Which key is sent is decided by the backend; the codes are repeated here to give
   them names. */
static const char* hard_key_name(uint32_t keycode)
{
    switch (keycode) {
    case 0x00000002: return "KEYCODE_PHONE_CALL";
    case 0x00000003: return "KEYCODE_PHONE_END";
    case 0x00000006: return "KEYCODE_SELECTOR_NEXT";
    case 0x00000007: return "KEYCODE_SELECTOR_PREVIOUS";
    case 0x0000000E: return "KEYCODE_BACK";
    case 0x0000000F: return "KEYCODE_SEEK_SUB";
    case 0x00000010: return "KEYCODE_SEEK_ADD";
    case 0x00000013: return "KEYCODE_MUTE";
    case 0x00000014: return "KEYCODE_OK";
    case 0x00000015: return "KEYCODE_MOVE_LEFT";
    case 0x00000016: return "KEYCODE_MOVE_RIGHT";
    case 0x0000001E: return "KEYCODE_MAIN";
    case 0x0000001F: return "KEYCODE_MEDIA_START";
    case 0x00000020: return "KEYCODE_MEDIA_STOP";
    case 0x00000021: return "KEYCODE_VR_START";
    case 0x00000022: return "KEYCODE_VR_STOP";
    default: return "unknown";
    }
}

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
            {
                CmdWriteLock lock;
                lib->cmdHUProtoclVersion(&version);
            }
            log_line("session.protocol_version_sent major=%u minor=%u", version.majorVersion,
                     version.minorVersion);

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
            {
                CmdWriteLock lock;
                lib->cmdVideoEncoderInit(&param);
            }
            log_line_debug("video.encoder_init_requested width=%u height=%u fps=%u", param.width,
                           param.height, param.frameRate);
            break;
        }
        case CMD_VIDEO_ENCODER_START: {
            CmdWriteLock lock;
            lib->cmdVideoEncoderStart();
            log_line_debug("video.encoder_started");
            break;
        }
        case CMD_VIDEO_ENCODER_PAUSE: {
            CmdWriteLock lock;
            lib->cmdVideoEncoderPause();
            log_line("video.encoder_paused");
            break;
        }
        case CMD_VIDEO_ENCODER_RESET: {
            CmdWriteLock lock;
            lib->cmdVideoEncoderReset();
            log_line("video.encoder_reset");
            break;
        }
        case CMD_GO_TO_FOREGROUND: {
            CmdWriteLock lock;
            lib->cmdGoToForeground();
            log_line("phone.foreground_requested");
            break;
        }
        case CMD_LAUNCH_MODE_NORMAL: {
            CmdWriteLock lock;
            lib->cmdLaunchModeNormal();
            break;
        }
        case CMD_LAUNCH_MODE_PHONE: {
            CmdWriteLock lock;
            lib->cmdLaunchModePhone();
            break;
        }
        case CMD_LAUNCH_MODE_MAP: {
            CmdWriteLock lock;
            lib->cmdLaunchModeMap();
            break;
        }
        case CMD_LAUNCH_MODE_MUSIC: {
            CmdWriteLock lock;
            lib->cmdLaunchModeMusic();
            break;
        }
        case CMD_AUTHEN_REQUEST:
            if (g_authen_result == 1) {
                log_line("auth.request_skipped reason=already_authenticated");
                break;
            }
            send_authen_request();
            break;
        default:
            log_line_warn("channel.unknown_tag ch=cmd tag=0x%02X action=ignore", (unsigned)tag);
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
            /* Every drag point logs one line, which floods a debug run. */
            /* log_line_debug("touch.sent action=%u x=%u y=%u", action.action, action.x, action.y); */
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
            log_line_debug("touch.sent action=down x=%u y=%u", point.x, point.y);
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
            log_line_debug("touch.sent action=up x=%u y=%u", point.x, point.y);
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
            log_line_debug("touch.sent action=move x=%u y=%u", point.x, point.y);
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
            log_line_debug("touch.sent action=click x=%u y=%u", point.x, point.y);
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
            log_line_debug("touch.sent action=double_click x=%u y=%u", point.x, point.y);
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
            log_line_debug("touch.sent action=long_press x=%u y=%u", point.x, point.y);
            break;
        }
        case CMD_HARD_KEY: {
            if (body_length < 4) {
                return;
            }
            S_TOUCH_CAR_HARD_KEY_CODE key;
            key.keycode = get_be32(body);
            lib->ctrlTouchCarHardKeyCode(&key);
            log_line_debug("key.sent name=%s code=0x%08X", hard_key_name(key.keycode), key.keycode);
            break;
        }
        default:
            log_line_warn("channel.unknown_tag ch=ctrl tag=0x%02X action=ignore", (unsigned)tag);
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
        log_line_warn("channel.unknown_tag ch=vr tag=0x%02X action=ignore", (unsigned)tag);
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
            log_line_warn("link.frame_rejected channel=%u bytes=%u channel_max=%u "
                          "action=drop_connection",
                          (unsigned)channel, payload_length, (unsigned)CL_MAX_PAYLOAD);
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
            log_line_error("link.accept_failed err=%s", strerror(errno));
            return NULL;
        }
        set_close_on_exec(fd);

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd >= 0) {
            ::close(g_client_fd);
        }
        g_client_fd = fd;
        pthread_mutex_unlock(&g_send_lock);
        log_line("link.backend_connected");

        broadcast_status();

        serve_client(fd);

        pthread_mutex_lock(&g_send_lock);
        if (g_client_fd == fd) {
            g_client_fd = -1;
        }
        pthread_mutex_unlock(&g_send_lock);
        ::close(fd);
        log_line("link.backend_disconnected action=wait");
    }
}

static int listen_unix(const char* path)
{
    ::unlink(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_line_error("link.socket_failed step=create err=%s", strerror(errno));
        return -1;
    }
    set_close_on_exec(fd);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (::bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        log_line_error("link.socket_failed step=bind path=%s err=%s", path, strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        log_line_error("link.socket_failed step=listen err=%s", strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

static void start_thread(void* (*entry)(void*), const char* name)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, entry, NULL) != 0) {
        log_line_error("thread.start_failed name=%s", name);
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
            "  --socket <path>    Unix socket the Go backend connects to (required)\n"
            "  --channel <name>   vendor channel number reported to the phone (required)\n"
            "  --cuid <id>        head unit id reported to the phone (required)\n"
            "  --host <address>   local address of the six channel ports (default 127.0.0.1)\n"
            "  --vid <0xXXXX>     target device VID (default: pick a phone)\n"
            "  --pid <0xXXXX>     target device PID (default: pick a phone)\n"
            "  --list             list the USB devices and exit, without touching any device\n"
            "  -q, --quiet        do not print the protocol stack's runtime logs\n"
            "  -v, --verbose      also emit the protocol stack's internal logs (discarded by default)\n"
            "  -h, --help         show this help\n"
            "\n"
            "The three required options are not needed by --list.\n",
            program);
}

int main(int argc, char** argv)
{
    const char* socket_path = NULL;
    const char* channel_host = "127.0.0.1";
    int vid = -1;
    int pid = -1;
    int list_only = 0;

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
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
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

    /* The Go backend owns these values and always passes them, so this process carries no
     * defaults of its own: a missing one would otherwise silently run with an identity
     * nobody configured. The device listing mode below never reaches the protocol stack
     * and does not need them. */
    if (!list_only) {
        const char* missing = NULL;
        if (socket_path == NULL) {
            missing = "--socket";
        } else if (g_channel.empty()) {
            missing = "--channel";
        } else if (g_cuid.empty()) {
            missing = "--cuid";
        }
        if (missing != NULL) {
            fprintf(stderr, "%s is required\n", missing);
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
    if (bridge_start(channel_host, vid, pid) != 0) {
        return 1;
    }

    g_listen_fd = listen_unix(socket_path);
    if (g_listen_fd < 0) {
        bridge_shutdown();
        return 1;
    }
    log_line("link.listening path=%s", socket_path);

    CCarLifeLib* lib = CCarLifeLib::getInstance();
    lib->carLifeLibInit();

    for (int attempt = 1; attempt <= 3; ++attempt) {
        g_connection_result = setup_channels();
        if (g_connection_result == 0) {
            break;
        }
        log_line_warn("session.channel_setup_failed attempt=%d code=%d retry_ms=1000", attempt,
                      g_connection_result);
        sleep(1);
    }
    if (g_connection_result != 0) {
        log_line_error("session.channel_setup_failed action=exit");
        ::close(g_listen_fd);
        ::unlink(socket_path);
        bridge_shutdown();
        return 1;
    }
    log_line("session.channels_connected count=6 addr=127.0.0.1");

    /* There is no control-channel receive thread: CTRL is a send-only channel here.
     * CTranRecvPackageProcess::receivePackageHead() has no CTRL_CHANNEL case, so it takes the
     * default branch, logs and returns true without reading the socket; receivePackageData()
     * likewise breaks out of the CTRL_CHANNEL case without reading. ctrlReceiveOperation()
     * therefore returns 0 immediately on every call, and a loop around it spins on one core:
     * measured a full core, roughly 360k flushed log lines per second into /dev/null. */
    start_thread(cmd_receive_thread, "command channel");
    start_thread(video_receive_thread, "video channel");
    start_thread(media_receive_thread, "audio channel");
    start_thread(tts_receive_thread, "TTS channel");
    start_thread(vr_receive_thread, "voice channel");
    start_thread(backend_loop, "backend");

    /* The vehicle data channel reports a position, which has to be sampled before the
     * phone asks whether this head unit has one: start the sampler first. */
    if (gps_start() != 0) {
        log_line_warn("gps.sampler_start_failed");
    }
    start_thread(gps_send_thread, "vehicle data");

    log_line("process.ready backend=waiting");

    /* The USB receive loop owns the main thread: it returns when the phone goes away, and
     * the process ends there so that carlifed rebuilds the link. */
    int rc = bridge_loop();

    bridge_shutdown();
    ::close(g_listen_fd);
    ::unlink(socket_path);
    return rc == 0 ? 0 : 1;
}
