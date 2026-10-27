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

/* ------------------------------------------------------ CarLife message log
 *
 * One line per CarLife message, written where the message crosses the boundary instead of
 * inside the handler that acts on it:
 *
 *   msg.send service_type=0x00018001(MSG_CMD_HU_PROTOCOL_VERSION) majorVersion=3 minorVersion=0
 *   msg.recv service_type=0x00010026(MSG_CMD_MODULE_STATUS) modules=1 moduleID_0=3(music) statusID_0=0(idle)
 *
 * send is this head unit to the phone, recv is the phone to this head unit. The area is msg
 * rather than cmd because one table covers all six channels the library carries:
 * MSG_MEDIA_INIT sits on the media channel (0x00030001) and MSG_TOUCH_ACTION on the ctrl
 * channel (0x00068001), so cmd would name two thirds of these lines wrong.
 *
 * The message is one field: the number the wire carries, its name in parentheses. An enum
 * field is written the same way, so a line reads without the tables at hand. A value the
 * tables do not cover keeps its own number and says (UNKNOWN) rather than borrowing the name
 * of a neighbour, which is how a value added by a newer phone shows up. The names are the
 * guide's and the vendor's (table 10.2-1 for the command channel, 10.3-2 for video, 10.4-2
 * for media and navi tts, 10.5-2 for vr, 10.6-3 for touch, ModuleStatusModel.java for the
 * module ids).
 *
 * A message whose row is missing from the table still produces a line, as
 * service_type=0x…(UNKNOWN), so that a gap in the table shows up instead of a message that
 * silently vanishes.
 *
 * The level in the table is the only place that decides whether a message is written: OFF
 * drops it before the fields are built, which is how the frame rate and drag rate messages
 * stay out of the log. A send that fails is written at error level whatever the message's
 * own level is, because the caller is waiting for the reply that never comes.
 *
 * A call site passes the value, not a format string: msg_send(MSG_..., rc, version)
 * formats through the formatter that the value's type resolves to. The field list therefore
 * sits next to the message it describes, and a format string and its arguments cannot drift
 * apart.
 */

typedef enum {
    MSG_LOG_OFF,   /* not written: the message is at frame or drag rate */
    MSG_LOG_DEBUG, /* written at debug level: driven by a timer */
    MSG_LOG_INFO,
} message_log_level;

typedef struct message_spec {
    uint32_t service_type;
    const char* name;
    message_log_level level;
    /* Builds the field list of the line; NULL for a message without a body, and for a
     * message whose row is OFF (its fields are never built). */
    void (*format)(char* out, size_t size, const void* value);
} message_spec;

/* One row per message: identifier, service type, level, formatter. The identifier used at
 * the call sites and the name the line carries both come from that row, so the number, the
 * name and the level cannot disagree. Rows are ordered by service type.
 *
 * MSG_MEDIA_STOP and MSG_MEDIA_SEEK are not in the guide's media table; their numbers
 * follow its numbering, which has 0x00030002 and 0x00030005 unassigned. The library has a
 * callback for each, so the phone does send them. */
#define CARLIFE_MESSAGES(X)                                                                        \
    X(MSG_CMD_PROTOCOL_VERSION_MATCH_STATUS, 0x00010002, MSG_LOG_INFO, format_protocol_match)       \
    X(MSG_CMD_MD_INFO, 0x00010004, MSG_LOG_INFO, format_md_info)                                   \
    X(MSG_CMD_MD_BT_OOB_INFO, 0x00010006, MSG_LOG_INFO, format_bt_pair_info)                       \
    X(MSG_CMD_VIDEO_ENCODER_INIT_DONE, 0x00010008, MSG_LOG_INFO, format_encoder_init)              \
    X(MSG_CMD_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE, 0x0001000D, MSG_LOG_INFO, format_frame_rate)   \
    X(MSG_CMD_TELE_STATE_CHANGE_INCOMING, 0x00010014, MSG_LOG_INFO, NULL)                          \
    X(MSG_CMD_TELE_STATE_CHANGE_OUTGOING, 0x00010015, MSG_LOG_INFO, NULL)                          \
    X(MSG_CMD_TELE_STATE_CHANGE_IDLE, 0x00010016, MSG_LOG_INFO, NULL)                              \
    X(MSG_CMD_TELE_STATE_CHANGE_INCALLING, 0x00010017, MSG_LOG_INFO, NULL)                         \
    X(MSG_CMD_SCREEN_ON, 0x00010018, MSG_LOG_INFO, NULL)                                           \
    X(MSG_CMD_SCREEN_OFF, 0x00010019, MSG_LOG_INFO, NULL)                                          \
    X(MSG_CMD_SCREEN_USERPRESENT, 0x0001001A, MSG_LOG_INFO, NULL)                                  \
    X(MSG_CMD_FOREGROUND, 0x0001001B, MSG_LOG_INFO, NULL)                                          \
    X(MSG_CMD_BACKGROUND, 0x0001001C, MSG_LOG_INFO, NULL)                                          \
    X(MSG_CMD_GO_TO_DESKTOP, 0x00010021, MSG_LOG_INFO, NULL)                                       \
    X(MSG_CMD_MIC_RECORD_WAKEUP_START, 0x00010022, MSG_LOG_INFO, NULL)                             \
    X(MSG_CMD_MIC_RECORD_END, 0x00010023, MSG_LOG_INFO, NULL)                                      \
    X(MSG_CMD_MIC_RECORD_RECOG_START, 0x00010024, MSG_LOG_INFO, NULL)                              \
    X(MSG_CMD_MODULE_STATUS, 0x00010026, MSG_LOG_INFO, format_module_status)                       \
    X(MSG_CMD_NAVI_NEXT_TURN_INFO, 0x00010030, MSG_LOG_INFO, format_navi_next_turn)                \
    X(MSG_CMD_CAR_DATA_SUBSCRIBE, 0x00010031, MSG_LOG_INFO, format_vehicle_info_list)              \
    X(MSG_CMD_CAR_DATA_SUBSCRIBE_START, 0x00010033, MSG_LOG_INFO, format_vehicle_info_list)        \
    X(MSG_CMD_MEDIA_INFO, 0x00010035, MSG_LOG_INFO, format_media_info)                             \
    X(MSG_CMD_MEDIA_PROGRESS_BAR, 0x00010036, MSG_LOG_DEBUG, format_media_progress)                \
    X(MSG_CMD_CONNECT_EXCEPTION, 0x00010037, MSG_LOG_INFO, format_connect_exception)               \
    X(MSG_CMD_MD_AUTHEN_RESPONSE, 0x00010049, MSG_LOG_INFO, format_authen_response)                \
    X(MSG_CMD_MD_AUTHEN_RESULT, 0x0001004B, MSG_LOG_INFO, format_authen_result)                    \
    X(MSG_CMD_MD_FEATURE_CONFIG_REQUEST, 0x00010051, MSG_LOG_INFO, NULL)                           \
    X(MSG_CMD_MD_EXIT, 0x00010059, MSG_LOG_INFO, NULL)                                             \
    X(MSG_CMD_HU_PROTOCOL_VERSION, 0x00018001, MSG_LOG_INFO, format_protocol_version)              \
    X(MSG_CMD_HU_INFO, 0x00018003, MSG_LOG_INFO, format_hu_info)                                   \
    X(MSG_CMD_VIDEO_ENCODER_INIT, 0x00018007, MSG_LOG_INFO, format_encoder_init)                   \
    X(MSG_CMD_VIDEO_ENCODER_START, 0x00018009, MSG_LOG_INFO, NULL)                                 \
    X(MSG_CMD_VIDEO_ENCODER_PAUSE, 0x0001800A, MSG_LOG_INFO, NULL)                                 \
    X(MSG_CMD_VIDEO_ENCODER_RESET, 0x0001800B, MSG_LOG_INFO, NULL)                                 \
    X(MSG_CMD_CAR_GPS, 0x00018010, MSG_LOG_DEBUG, format_gps)                                      \
    X(MSG_CMD_LAUNCH_MODE_NORMAL, 0x0001801D, MSG_LOG_INFO, NULL)                                  \
    X(MSG_CMD_LAUNCH_MODE_PHONE, 0x0001801E, MSG_LOG_INFO, NULL)                                   \
    X(MSG_CMD_LAUNCH_MODE_MAP, 0x0001801F, MSG_LOG_INFO, NULL)                                     \
    X(MSG_CMD_LAUNCH_MODE_MUSIC, 0x00018020, MSG_LOG_INFO, NULL)                                   \
    X(MSG_CMD_GO_TO_FOREGROUND, 0x00018025, MSG_LOG_INFO, NULL)                                    \
    X(MSG_CMD_STATISTIC_INFO, 0x00018027, MSG_LOG_INFO, format_statistic_info)                     \
    X(MSG_CMD_CAR_DATA_SUBSCRIBE_DONE, 0x00018032, MSG_LOG_INFO, format_vehicle_info_list)         \
    X(MSG_CMD_CAR_DATA_SUBSCRIBE_STOP, 0x00018034, MSG_LOG_INFO, format_vehicle_info_list)         \
    X(MSG_CMD_HU_AUTHEN_REQUEST, 0x00018048, MSG_LOG_INFO, format_authen_request)                  \
    X(MSG_CMD_HU_AUTHEN_RESULT, 0x0001804A, MSG_LOG_INFO, format_authen_result)                    \
    X(MSG_CMD_HU_FEATURE_CONFIG_RESPONSE, 0x00018052, MSG_LOG_INFO, format_feature_config)         \
    X(MSG_VIDEO_DATA, 0x00020001, MSG_LOG_OFF, format_data_chunk)                                  \
    X(MSG_VIDEO_HEARTBEAT, 0x00020002, MSG_LOG_OFF, NULL)                                          \
    X(MSG_MEDIA_INIT, 0x00030001, MSG_LOG_INFO, format_audio_init)                                 \
    X(MSG_MEDIA_STOP, 0x00030002, MSG_LOG_INFO, NULL)                                              \
    X(MSG_MEDIA_PAUSE, 0x00030003, MSG_LOG_INFO, NULL)                                             \
    X(MSG_MEDIA_RESUME_PLAY, 0x00030004, MSG_LOG_INFO, NULL)                                       \
    X(MSG_MEDIA_SEEK, 0x00030005, MSG_LOG_INFO, NULL)                                              \
    X(MSG_MEDIA_DATA, 0x00030006, MSG_LOG_OFF, format_data_chunk)                                  \
    X(MSG_NAVI_TTS_INIT, 0x00040001, MSG_LOG_INFO, format_audio_init)                              \
    X(MSG_NAVI_TTS_END, 0x00040002, MSG_LOG_INFO, NULL)                                            \
    X(MSG_NAVI_TTS_DATA, 0x00040003, MSG_LOG_OFF, format_data_chunk)                               \
    X(MSG_VR_AUDIO_INIT, 0x00050002, MSG_LOG_INFO, format_audio_init)                              \
    X(MSG_VR_AUDIO_DATA, 0x00050003, MSG_LOG_OFF, format_data_chunk)                               \
    X(MSG_VR_AUDIO_STOP, 0x00050004, MSG_LOG_INFO, NULL)                                           \
    X(MSG_TOUCH_ACTION, 0x00068001, MSG_LOG_OFF, NULL)                                             \
    X(MSG_TOUCH_ACTION_DOWN, 0x00068002, MSG_LOG_OFF, NULL)                                        \
    X(MSG_TOUCH_ACTION_UP, 0x00068003, MSG_LOG_OFF, NULL)                                          \
    X(MSG_TOUCH_ACTION_MOVE, 0x00068004, MSG_LOG_OFF, NULL)                                        \
    X(MSG_TOUCH_SINGLE_CLICK, 0x00068005, MSG_LOG_OFF, NULL)                                       \
    X(MSG_TOUCH_DOUBLE_CLICK, 0x00068006, MSG_LOG_OFF, NULL)                                       \
    X(MSG_TOUCH_LONG_PRESS, 0x00068007, MSG_LOG_OFF, NULL)                                         \
    X(MSG_TOUCH_CAR_HARD_KEY_CODE, 0x00068008, MSG_LOG_INFO, format_hard_key)

/* Defined with the helpers they belong to further down; two field lists name a value here. */
static const char* hard_key_name(uint32_t keycode);
static const char* media_play_mode_name(uint32_t mode);

/* Builds the field list of a line. A formatter names fields and leaves the separators, the
 * quoting and the bounds to this writer: fields are written as key=value separated by one
 * space, so a line reads the same way as every other line in this log.
 *
 * A value that can contain spaces is quoted and escaped; numbers and the identifiers the
 * guide names are written bare. A list repeats the field name with the entry's index
 * appended (moduleID_0, moduleID_1), which keeps the entries apart without nesting and
 * without a second level of punctuation.
 *
 * A list that does not fit is cut where it stops and the cut is marked with three dots, so
 * a truncated line is not mistaken for a complete one. */
struct field_writer {
    field_writer(char* out, size_t size) : out_(out), size_(size), used_(0), truncated_(false)
    {
        out_[0] = '\0';
    }

    /* The formatter has finished writing, so a field list that ran out of room can be marked
     * now; doing it on the way out keeps every formatter free of the bookkeeping. */
    ~field_writer()
    {
        if (truncated_ && size_ >= 4) {
            memcpy(out_ + size_ - 4, "...", 3);
        }
    }

    void u32(const char* key, uint32_t value) { append("%s=%u", key, value); }
    void i32(const char* key, int32_t value) { append("%s=%d", key, value); }
    void u64(const char* key, uint64_t value) { append("%s=%llu", key, (unsigned long long)value); }
    void boolean(const char* key, bool value) { append("%s=%s", key, value ? "true" : "false"); }
    /* Text is quoted: a song title has spaces in it, and without the quotes the field after
     * it could not be told apart from the title. The escape is the one json_quote applies,
     * which is also what a reader of this log expects for an embedded quote or a newline. */
    void text(const char* key, const std::string& value) { append("%s=%s", key, json_quote(value).c_str()); }
    /* An enum: the number the wire carries, the name the tables give it in parentheses. The
     * number stays because that is what a raw capture shows; the name because it is what
     * makes the line readable. A value the tables do not cover says UNKNOWN. */
    void enum_u32(const char* key, uint32_t value, const char* name)
    {
        append("%s=%u(%s)", key, value, name);
    }

    /* Key codes are the one enum whose values are only ever quoted in hex (table 10.6-1),
     * so the number is written the way the table and the vendor sources write it. */
    void enum_hex(const char* key, uint32_t value, const char* name)
    {
        append("%s=0x%08X(%s)", key, value, name);
    }

    /* The same field repeated once per entry of a list. */
    void entry_u32(const char* key, unsigned index, uint32_t value) { append("%s_%u=%u", key, index, value); }

    void entry_enum_u32(const char* key, unsigned index, uint32_t value, const char* name)
    {
        append("%s_%u=%u(%s)", key, index, value, name);
    }

    void entry_enum_i32(const char* key, unsigned index, int32_t value, const char* name)
    {
        append("%s_%u=%d(%s)", key, index, value, name);
    }

  private:
    void append(const char* fmt, ...)
    {
        /* Two bytes are needed before formatting: the separator and one more character. */
        if (truncated_ || used_ + 2 > size_) {
            truncated_ = true;
            return;
        }
        if (used_ > 0) {
            out_[used_++] = ' ';
            out_[used_] = '\0';
        }
        va_list args;
        va_start(args, fmt);
        int written = vsnprintf(out_ + used_, size_ - used_, fmt, args);
        va_end(args);
        if (written < 0) {
            truncated_ = true;
            return;
        }
        if ((size_t)written >= size_ - used_) {
            truncated_ = true;
            used_ = size_ - 1;
            return;
        }
        used_ += (size_t)written;
    }

    char* out_;
    size_t size_;
    size_t used_;
    bool truncated_;
};

/* A body this side does not parse (a PCM or video chunk): only its length is worth a line,
 * and the message's level keeps even that out of the log unless it is turned on. */
struct data_chunk {
    uint32_t bytes;
};

static void format_protocol_version(char* out, size_t size, const void* value)
{
    const S_HU_PROTOCOL_VERSION* version = (const S_HU_PROTOCOL_VERSION*)value;
    field_writer w(out, size);
    w.u32("majorVersion", version->majorVersion);
    w.u32("minorVersion", version->minorVersion);
}

static void format_protocol_match(char* out, size_t size, const void* value)
{
    const S_PROTOCOL_VERSION_MATCH_SATUS* status = (const S_PROTOCOL_VERSION_MATCH_SATUS*)value;
    field_writer w(out, size);
    w.u32("matchStatus", status->matchStatus);
}

/* MSG_CMD_HU_INFO and MSG_CMD_MD_INFO carry the same structure, and this head unit fills all
 * of it, so both lines list all twenty-three fields rather than the six the guide lists for
 * the head unit. An empty field is printed as an empty string: it states that the field is
 * there and empty, which is not the same as a field that was left out. */
template <typename T>
static void format_device_fields(char* out, size_t size, const T& info)
{
    field_writer w(out, size);
    w.text("os", info.os);
    w.text("board", info.board);
    w.text("bootloader", info.bootloader);
    w.text("brand", info.brand);
    w.text("cpu_abi", info.cpu_abi);
    w.text("cpu_abi2", info.cpu_abi2);
    w.text("device", info.device);
    w.text("display", info.display);
    w.text("fingerprint", info.fingerprint);
    w.text("hardware", info.hardware);
    w.text("host", info.host);
    w.text("cid", info.cid);
    w.text("manufacturer", info.manufacturer);
    w.text("model", info.model);
    w.text("product", info.product);
    w.text("serial", info.serial);
    w.text("codename", info.codename);
    w.text("incremental", info.incremental);
    w.text("release", info.release);
    w.text("sdk", info.sdk);
    w.u32("sdk_int", info.sdk_int);
    w.text("token", info.token);
    w.text("btaddress", info.btaddress);
}

static void format_hu_info(char* out, size_t size, const void* value)
{
    format_device_fields(out, size, *(const S_HU_INFO*)value);
}

static void format_md_info(char* out, size_t size, const void* value)
{
    format_device_fields(out, size, *(const S_MD_INFO*)value);
}

/* The guide names the first field address for HU->MD and btAddress for MD->HU; the
 * structure only has address, and this is the MD->HU direction. */
static void format_bt_pair_info(char* out, size_t size, const void* value)
{
    const S_BT_PAIR_INFO* info = (const S_BT_PAIR_INFO*)value;
    field_writer w(out, size);
    w.text("btAddress", info->address);
    w.text("passKey", info->passKey);
    w.text("hash", info->hash);
    w.text("randomizer", info->randomizer);
    w.text("uuid", info->uuid);
    w.text("name", info->name);
    w.u32("status", info->status);
}

/* MSG_CMD_VIDEO_ENCODER_INIT and its _DONE reply carry the same three fields, in structures
 * that are separate types with the same layout. */
static void format_encoder_init(char* out, size_t size, const void* value)
{
    const S_VIDEO_ENCODER_INIT* init = (const S_VIDEO_ENCODER_INIT*)value;
    field_writer w(out, size);
    w.u32("width", init->width);
    w.u32("height", init->height);
    w.u32("frameRate", init->frameRate);
}

static void format_frame_rate(char* out, size_t size, const void* value)
{
    const S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE* change =
        (const S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE*)value;
    field_writer w(out, size);
    w.u32("frameRate", change->frameRate);
}

/* Module ids, from ModuleStatusModel.java of the CarLife vehicle library. The names are the
 * short ones the phone uses for itself in the modules it reports. */
static const char* module_name(uint32_t module_id)
{
    switch (module_id) {
    case 1: return "phone";
    case 2: return "navi";
    case 3: return "music";
    case 4: return "voice";
    case 5: return "connect";
    case 6: return "mic";
    default: return "UNKNOWN";
    }
}

/* The status a module reports, named per module: the same number means something different
 * in each, so the name needs both. music is the one that matters for audio - it reports
 * running only while the phone is streaming program audio, so an idle music module means no
 * audio channel traffic is coming. */
static const char* module_status_name(uint32_t module_id, uint32_t status_id)
{
    static const char* const kPhone[] = {"idle", "incoming", "outgoing"};
    static const char* const kNavi[] = {"idle", "start", "stop"};
    static const char* const kMusic[] = {"idle", "running"};
    static const char* const kVoice[] = {"idle", "running", "mic-unsupported"};
    static const char* const kMic[] = {"vehicle-mic", "phone-mic", "unsupported"};
    const char* const* names = NULL;
    size_t count = 0;
    switch (module_id) {
    case 1: names = kPhone; count = sizeof(kPhone) / sizeof(kPhone[0]); break;
    case 2: names = kNavi; count = sizeof(kNavi) / sizeof(kNavi[0]); break;
    case 3: names = kMusic; count = sizeof(kMusic) / sizeof(kMusic[0]); break;
    case 4: names = kVoice; count = sizeof(kVoice) / sizeof(kVoice[0]); break;
    case 6: names = kMic; count = sizeof(kMic) / sizeof(kMic[0]); break;
    }
    if (names == NULL || status_id >= count) {
        return "UNKNOWN";
    }
    return names[status_id];
}

/* The module list is a chain; the count comes first so that a line cut short still says how
 * many entries it was supposed to carry. */
static void format_module_status(char* out, size_t size, const void* value)
{
    const S_MODULE_STATUS_LIST_MOBILE* list = (const S_MODULE_STATUS_LIST_MOBILE*)value;
    field_writer w(out, size);
    w.u32("modules", (uint32_t)list->cnt);
    unsigned i = 0;
    for (const S_MODULE_STATUS_MOBILE* node = list->moduleStatus; node != NULL && i < (unsigned)list->cnt;
         node = node->next, ++i) {
        w.entry_enum_u32("moduleID", i, node->moduleID, module_name(node->moduleID));
        w.entry_enum_u32("statusID", i, node->statusID,
                         module_status_name(node->moduleID, node->statusID));
    }
}

static void format_navi_next_turn(char* out, size_t size, const void* value)
{
    const S_NAVI_NEXT_TURN_INFO* info = (const S_NAVI_NEXT_TURN_INFO*)value;
    field_writer w(out, size);
    w.u32("action", info->action);
    w.u32("nextTurn", info->nextTurn);
    w.text("roadName", info->roadName);
    w.u32("totalDistance", info->totalDistance);
    w.u32("remainDistance", info->remainDistance);
    w.u32("time", info->time);
}

/* The vehicle data list, in all three directions it appears: the request the phone sends,
 * the reply this side sends, and the start/stop that follow. The structure calls the second
 * field flag; the guide calls it supportFlag and gives it the coordinate format, 1 for WGS84
 * and 2 for GCJ-02. */
static const char* coordinate_format_name(int32_t flag)
{
    switch (flag) {
    case 1: return "WGS84";
    case 2: return "GCJ-02";
    default: return "UNKNOWN";
    }
}

/* The vehicle data channel numbers its modules in a set of its own, not the one the module
 * status messages use: 0 is the position, which is the only module this head unit declares
 * (kCarDataGps below). Any other id is left unnamed rather than read against that other
 * table. */
static const char* vehicle_module_name(uint32_t module_id)
{
    return module_id == 0 ? "GPS" : "UNKNOWN";
}

static void format_vehicle_info_list(char* out, size_t size, const void* value)
{
    const S_VEHICLE_INFO_LIST* list = (const S_VEHICLE_INFO_LIST*)value;
    field_writer w(out, size);
    w.u32("modules", (uint32_t)list->cnt);
    unsigned i = 0;
    for (const S_VEHICLE_INFO* info = list->pVehicleInfo;
         info != NULL && i < (unsigned)list->cnt; info = info->pNext, ++i) {
        w.entry_enum_u32("moduleID", i, info->moduleID, vehicle_module_name(info->moduleID));
        w.entry_enum_i32("supportFlag", i, info->flag, coordinate_format_name(info->flag));
        w.entry_u32("frequency", i, info->frequency);
    }
}

/* albumArt is the cover bitmap, not text: the field carries its length in bytes, which is
 * enough to tell a missing cover from a present one. */
static void format_media_info(char* out, size_t size, const void* value)
{
    const S_MEDIA_INFO* info = (const S_MEDIA_INFO*)value;
    field_writer w(out, size);
    w.text("source", info->source);
    w.text("song", info->song);
    w.text("artist", info->artist);
    w.text("album", info->album);
    w.u32("albumArt_bytes", (uint32_t)info->albumArt.size());
    w.u32("duration", info->duration);
    w.u32("playlistNum", info->playlistNum);
    w.text("songId", info->songId);
    w.enum_u32("mode", info->mode, media_play_mode_name(info->mode));
}

static void format_media_progress(char* out, size_t size, const void* value)
{
    const S_MEDIA_PROGRESS_BAR* progress = (const S_MEDIA_PROGRESS_BAR*)value;
    field_writer w(out, size);
    w.u32("progressBar", progress->progressBar);
}

static void format_connect_exception(char* out, size_t size, const void* value)
{
    const S_CONNECTION_EXCEPTION* exception = (const S_CONNECTION_EXCEPTION*)value;
    field_writer w(out, size);
    w.u32("exceptionType", exception->exceptionType);
}

static void format_authen_request(char* out, size_t size, const void* value)
{
    const S_AUTHEN_REQUEST* request = (const S_AUTHEN_REQUEST*)value;
    field_writer w(out, size);
    w.text("randomValue", request->randomValue);
}

static void format_authen_response(char* out, size_t size, const void* value)
{
    const S_AUTHEN_RESPONSE* response = (const S_AUTHEN_RESPONSE*)value;
    field_writer w(out, size);
    w.text("encryptValue", response->encryptValue);
}

/* The guide writes the field of MSG_CMD_HU_AUTHEN_RESULT as result, while the structure and
 * the serialized message both call it authenResult. The line uses authenResult, so it
 * matches what is on the wire. */
static void format_authen_result(char* out, size_t size, const void* value)
{
    const S_HU_AUTHEN_RESULT* result = (const S_HU_AUTHEN_RESULT*)value;
    field_writer w(out, size);
    w.boolean("authenResult", result->authenResult);
}

/* The list is a chain of count entries, and the keys are the customization names of the
 * guide's list 10.2-2, so the fields read as that table. */
static void format_feature_config(char* out, size_t size, const void* value)
{
    const S_FEATURE_CONFIG_LIST* list = (const S_FEATURE_CONFIG_LIST*)value;
    field_writer w(out, size);
    w.u32("features", (uint32_t)list->cnt);
    unsigned i = 0;
    for (const S_FEATURE_CONFIG* item = list->pFeatureConfig; item != NULL && i < (unsigned)list->cnt;
         item = item->pNext, ++i) {
        w.u32(item->key.c_str(), item->value);
    }
}

/* Of the twenty-five fields of MSG_CMD_CAR_GPS this head unit only has a source for these
 * seven; the rest (signal quality, satellite counts, the DOPs, the per-axis speeds) are sent
 * as zero and would only make the line longer. The values are the wire values: latitude and
 * longitude in 1/1,000,000 degrees, height in 0.1 m, speed in 0.01 km/h, heading in 0.1
 * degrees. antennaState is unsigned on the wire and -1 means there is no fix. */
static void format_gps(char* out, size_t size, const void* value)
{
    const S_CAR_GPS* gps = (const S_CAR_GPS*)value;
    field_writer w(out, size);
    w.i32("antennaState", (int32_t)gps->antennaState);
    w.u32("latitude", gps->latitude);
    w.u32("longitude", gps->longitude);
    w.u32("height", gps->height);
    w.u32("speed", gps->speed);
    w.u32("heading", gps->heading);
    w.u64("timeStamp", gps->timeStamp);
}

static void format_statistic_info(char* out, size_t size, const void* value)
{
    const S_STATISTICS_INFO* info = (const S_STATISTICS_INFO*)value;
    field_writer w(out, size);
    w.text("cuid", info->cuid);
    w.text("versionName", info->versionName);
    w.u32("versionCode", info->versionCode);
    w.text("channel", info->channel);
    w.u32("connectCount", info->connectCount);
    w.u32("connectSuccessCount", info->connectSuccessCount);
    w.u32("connectTime", info->connectTime);
    w.text("crashLog", info->crashLog);
}

static void format_data_chunk(char* out, size_t size, const void* value)
{
    const data_chunk* chunk = (const data_chunk*)value;
    field_writer w(out, size);
    w.u32("bytes", chunk->bytes);
}

/* media, navi tts and vr each send the same initialization: rate, channel layout and sample
 * format, in one structure. */
static void format_audio_init(char* out, size_t size, const void* value)
{
    const S_AUDIO_INIT_PARAMETER* param = (const S_AUDIO_INIT_PARAMETER*)value;
    field_writer w(out, size);
    w.u32("sampleRate", param->sampleRate);
    w.u32("channelConfig", param->channelConfig);
    w.u32("sampleFormat", param->sampleFormat);
}

static void format_hard_key(char* out, size_t size, const void* value)
{
    const S_TOUCH_CAR_HARD_KEY_CODE* key = (const S_TOUCH_CAR_HARD_KEY_CODE*)value;
    field_writer w(out, size);
    w.enum_hex("keycode", key->keycode, hard_key_name(key->keycode));
}

/* The identifiers below are what the call sites use, and the table rows above give them
 * their values: a message is named, numbered and levelled in exactly one place. */
enum {
#define CARLIFE_MESSAGE_ID(name, service_type, level, format) name = service_type,
    CARLIFE_MESSAGES(CARLIFE_MESSAGE_ID)
#undef CARLIFE_MESSAGE_ID
};

static const message_spec kMessageTable[] = {
#define CARLIFE_MESSAGE_ROW(name, service_type, level, format) {service_type, #name, level, format},
    CARLIFE_MESSAGES(CARLIFE_MESSAGE_ROW)
#undef CARLIFE_MESSAGE_ROW
};

/* Long enough for MSG_CMD_MD_INFO, the longest field list here: twenty-three fields, of which
 * the phone fills twenty. */
#define LOG_FIELDS_MAX 1536

static const message_spec* find_message(uint32_t service_type)
{
    for (size_t i = 0; i < sizeof(kMessageTable) / sizeof(kMessageTable[0]); ++i) {
        if (kMessageTable[i].service_type == service_type) {
            return &kMessageTable[i];
        }
    }
    return NULL;
}

/* A message this file handles but the table does not list. Written rather than dropped: the
 * row is what carries the level and the formatter, so a missing row would otherwise take
 * the message out of the log without leaving a trace. */
static void log_unlisted_message(const char* direction, uint32_t service_type)
{
    log_line_warn("msg.%s service_type=0x%08X(UNKNOWN)", direction, service_type);
}

static void write_message_line(const message_spec* spec, const char* direction, int rc,
                               const char* fields)
{
    /* The fields follow the service type directly; a message without a body ends there. A
     * failed send brings rc forward, so that the reason it is an error line comes before the
     * body it never delivered. */
    const char* gap = fields[0] != '\0' ? " " : "";
    if (rc < 0) {
        log_line_error("msg.%s service_type=0x%08X(%s) rc=%d%s%s", direction, spec->service_type,
                       spec->name, rc, gap, fields);
    } else if (spec->level == MSG_LOG_DEBUG) {
        log_line_debug("msg.%s service_type=0x%08X(%s)%s%s", direction, spec->service_type,
                       spec->name, gap, fields);
    } else if (spec->level == MSG_LOG_INFO) {
        log_line("msg.%s service_type=0x%08X(%s)%s%s", direction, spec->service_type, spec->name,
                 gap, fields);
    }
}

static void msg_recv(uint32_t service_type)
{
    const message_spec* spec = find_message(service_type);
    if (spec == NULL) {
        log_unlisted_message("recv", service_type);
        return;
    }
    write_message_line(spec, "recv", 0, "");
}

static void msg_send(uint32_t service_type, int rc)
{
    const message_spec* spec = find_message(service_type);
    if (spec == NULL) {
        log_unlisted_message("send", service_type);
        return;
    }
    write_message_line(spec, "send", rc, "");
}

template <typename T>
static void msg_recv(uint32_t service_type, const T& value)
{
    const message_spec* spec = find_message(service_type);
    if (spec == NULL) {
        log_unlisted_message("recv", service_type);
        return;
    }
    if (spec->level == MSG_LOG_OFF) {
        return;
    }
    char fields[LOG_FIELDS_MAX];
    if (spec->format != NULL) {
        spec->format(fields, sizeof(fields), &value);
    } else {
        fields[0] = '\0';
    }
    write_message_line(spec, "recv", 0, fields);
}

template <typename T>
static void msg_send(uint32_t service_type, int rc, const T& value)
{
    const message_spec* spec = find_message(service_type);
    if (spec == NULL) {
        log_unlisted_message("send", service_type);
        return;
    }
    /* A failed send is written whatever the level is: the caller is waiting for a reply
     * that will not come, and the level of a successful message says nothing about that. */
    if (rc >= 0 && spec->level == MSG_LOG_OFF) {
        return;
    }
    char fields[LOG_FIELDS_MAX];
    if (spec->format != NULL) {
        spec->format(fields, sizeof(fields), &value);
    } else {
        fields[0] = '\0';
    }
    write_message_line(spec, "send", rc, fields);
}

/* Sends one message under the command write lock and records it. Every command channel write
 * needs that lock (see g_cmd_write_lock), so taking it here leaves a call site with nothing
 * to do but name the message and pass the value. */
#define SEND_MESSAGE(message, call, ...)                                                           \
    do {                                                                                           \
        int rc_;                                                                                   \
        {                                                                                          \
            CmdWriteLock lock_;                                                                    \
            rc_ = (call);                                                                          \
        }                                                                                          \
        msg_send(message, rc_, ##__VA_ARGS__);                                                 \
    } while (0)

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
    msg_recv(MSG_CMD_PROTOCOL_VERSION_MATCH_STATUS, *status);
    uint8_t buffer[4];
    put_be32(buffer, status->matchStatus);
    send_frame(CH_CMD, EV_PROTOCOL_MATCH, buffer, sizeof(buffer));
}

static void cb_md_info(S_MD_INFO* info)
{
    msg_recv(MSG_CMD_MD_INFO, *info);

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
    msg_recv(MSG_CMD_MD_BT_OOB_INFO, *info);
}

static void cb_video_encoder_init_done(S_VIDEO_ENCODER_INIT_DONE* info)
{
    uint8_t buffer[12];
    put_be32(buffer, info->width);
    put_be32(buffer + 4, info->height);
    put_be32(buffer + 8, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_INIT_DONE, buffer, sizeof(buffer));
    msg_recv(MSG_CMD_VIDEO_ENCODER_INIT_DONE, *info);
}

static void cb_video_frame_rate_done(S_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE* info)
{
    uint8_t buffer[4];
    put_be32(buffer, info->frameRate);
    send_frame(CH_CMD, EV_VIDEO_FPS_DONE, buffer, sizeof(buffer));
    msg_recv(MSG_CMD_VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE, *info);
}

static void cb_tele_state(uint8_t state)
{
    send_frame(CH_CMD, EV_TELE_STATE, &state, 1);
}

static void cb_tele_incoming(void)
{
    msg_recv(MSG_CMD_TELE_STATE_CHANGE_INCOMING);
    cb_tele_state(1);
}
static void cb_tele_outgoing(void)
{
    msg_recv(MSG_CMD_TELE_STATE_CHANGE_OUTGOING);
    cb_tele_state(2);
}
static void cb_tele_idle(void)
{
    msg_recv(MSG_CMD_TELE_STATE_CHANGE_IDLE);
    cb_tele_state(0);
}
static void cb_tele_in_calling(void)
{
    msg_recv(MSG_CMD_TELE_STATE_CHANGE_INCALLING);
    cb_tele_state(3);
}

static void cb_screen_on(void)
{
    send_frame(CH_CMD, EV_SCREEN_ON, NULL, 0);
    msg_recv(MSG_CMD_SCREEN_ON);
}

static void cb_screen_off(void)
{
    send_frame(CH_CMD, EV_SCREEN_OFF, NULL, 0);
    msg_recv(MSG_CMD_SCREEN_OFF);
}

static void cb_screen_user_present(void)
{
    msg_recv(MSG_CMD_SCREEN_USERPRESENT);
}

static void cb_foreground(void)
{
    send_frame(CH_CMD, EV_FOREGROUND, NULL, 0);
    msg_recv(MSG_CMD_FOREGROUND);
}

static void cb_background(void)
{
    send_frame(CH_CMD, EV_BACKGROUND, NULL, 0);
    msg_recv(MSG_CMD_BACKGROUND);
}

static void cb_go_to_desktop(void)
{
    send_frame(CH_CMD, EV_GO_TO_DESKTOP, NULL, 0);
    msg_recv(MSG_CMD_GO_TO_DESKTOP);
}

static void cb_mic_wakeup(void)
{
    send_frame(CH_CMD, EV_MIC_WAKEUP, NULL, 0);
    msg_recv(MSG_CMD_MIC_RECORD_WAKEUP_START);
}

static void cb_mic_end(void)
{
    send_frame(CH_CMD, EV_MIC_END, NULL, 0);
    msg_recv(MSG_CMD_MIC_RECORD_END);
}

static void cb_mic_recog(void)
{
    send_frame(CH_CMD, EV_MIC_RECOG, NULL, 0);
    msg_recv(MSG_CMD_MIC_RECORD_RECOG_START);
}

static void cb_module_status(S_MODULE_STATUS_LIST_MOBILE* list)
{
    msg_recv(MSG_CMD_MODULE_STATUS, *list);
}

static void cb_navi_next_turn(S_NAVI_NEXT_TURN_INFO* info)
{
    msg_recv(MSG_CMD_NAVI_NEXT_TURN_INFO, *info);
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

    SEND_MESSAGE(MSG_CMD_STATISTIC_INFO, CCarLifeLib::getInstance()->cmdStatisticInfo(&info), info);
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

    SEND_MESSAGE(MSG_CMD_HU_INFO, CCarLifeLib::getInstance()->cmdHUInfro(&info), info);
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

    SEND_MESSAGE(MSG_CMD_HU_AUTHEN_REQUEST,
                 CCarLifeLib::getInstance()->cmdHuAuthenRequest(&request), request);
}

static void cb_md_authen_response(S_AUTHEN_RESPONSE* response)
{
    msg_recv(MSG_CMD_MD_AUTHEN_RESPONSE, *response);

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
    SEND_MESSAGE(MSG_CMD_HU_AUTHEN_RESULT, CCarLifeLib::getInstance()->cmdHuAuthenResult(&result),
                 result);
}

static void cb_md_authen_result(S_MD_AUTHEN_RESULT* result)
{
    g_authen_result = result->authenResult ? 1 : 0;
    msg_recv(MSG_CMD_MD_AUTHEN_RESULT, *result);
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
    msg_recv(MSG_CMD_MD_FEATURE_CONFIG_REQUEST);

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
    SEND_MESSAGE(MSG_CMD_HU_FEATURE_CONFIG_RESPONSE,
                 CCarLifeLib::getInstance()->cmdFeatureConfigResponse(&g_feature_config_list),
                 g_feature_config_list);

    send_frame(CH_CMD, EV_FEATURE_CONFIG_REQUEST, NULL, 0);
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
    default: return "UNKNOWN";
    }
}

/* What the phone's media player is doing, reported on the command channel: the track
 * whenever it changes and the position once a second while it plays. Neither drives
 * anything, but they are the only way to tell whether the phone's music module is really
 * running - which is what decides whether the audio channel carries anything at all. The
 * fields of MSG_CMD_MEDIA_INFO (guide 13.2) are listed in format_media_info. */
static void cb_media_info(S_MEDIA_INFO* info)
{
    msg_recv(MSG_CMD_MEDIA_INFO, *info);
}

static void cb_media_progress_bar(S_MEDIA_PROGRESS_BAR* progress)
{
    msg_recv(MSG_CMD_MEDIA_PROGRESS_BAR, *progress);
}

static void cb_connect_exception(S_CONNECTION_EXCEPTION* exception)
{
    msg_recv(MSG_CMD_CONNECT_EXCEPTION, *exception);
}

static void cb_md_exit(void)
{
    send_frame(CH_CMD, EV_MD_EXIT, NULL, 0);
    msg_recv(MSG_CMD_MD_EXIT);
}

/* ------------------------------------------------------- Audio/video channels */

static void send_audio_init(uint8_t channel, S_AUDIO_INIT_PARAMETER* param)
{
    uint8_t buffer[12];
    put_be32(buffer, param->sampleRate);
    put_be32(buffer + 4, param->channelConfig);
    put_be32(buffer + 8, param->sampleFormat);
    send_frame(channel, EV_AUDIO_INIT, buffer, sizeof(buffer));
}

static void cb_video_data(u8* data, u32 length)
{
    send_frame(CH_VIDEO, EV_VIDEO_DATA, data, length);
    msg_recv(MSG_VIDEO_DATA, data_chunk{length});
}

static void cb_video_heartbeat(void)
{
    send_frame(CH_VIDEO, EV_VIDEO_HEARTBEAT, NULL, 0);
    msg_recv(MSG_VIDEO_HEARTBEAT);
}

static void cb_media_init(S_AUDIO_INIT_PARAMETER* param)
{
    msg_recv(MSG_MEDIA_INIT, *param);
    send_audio_init(CH_AUDIO, param);
}
static void cb_media_data(u8* data, u32 length)
{
    send_frame(CH_AUDIO, EV_AUDIO_DATA, data, length);
    msg_recv(MSG_MEDIA_DATA, data_chunk{length});
}
static void cb_media_stop(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_STOP, NULL, 0);
    msg_recv(MSG_MEDIA_STOP);
}
static void cb_media_pause(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_PAUSE, NULL, 0);
    msg_recv(MSG_MEDIA_PAUSE);
}
static void cb_media_resume(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_RESUME, NULL, 0);
    msg_recv(MSG_MEDIA_RESUME_PLAY);
}
static void cb_media_seek(void)
{
    send_frame(CH_AUDIO, EV_AUDIO_SEEK, NULL, 0);
    msg_recv(MSG_MEDIA_SEEK);
}

static void cb_tts_init(S_AUDIO_INIT_PARAMETER* param)
{
    msg_recv(MSG_NAVI_TTS_INIT, *param);
    send_audio_init(CH_TTS, param);
}
static void cb_tts_data(u8* data, u32 length)
{
    send_frame(CH_TTS, EV_AUDIO_DATA, data, length);
    msg_recv(MSG_NAVI_TTS_DATA, data_chunk{length});
}
static void cb_tts_stop(void)
{
    send_frame(CH_TTS, EV_AUDIO_STOP, NULL, 0);
    msg_recv(MSG_NAVI_TTS_END);
}

static void cb_vr_init(S_AUDIO_INIT_PARAMETER* param)
{
    msg_recv(MSG_VR_AUDIO_INIT, *param);
    send_audio_init(CH_VR, param);
}
static void cb_vr_data(u8* data, u32 length)
{
    send_frame(CH_VR, EV_AUDIO_DATA, data, length);
    msg_recv(MSG_VR_AUDIO_DATA, data_chunk{length});
}
static void cb_vr_stop(void)
{
    send_frame(CH_VR, EV_AUDIO_STOP, NULL, 0);
    msg_recv(MSG_VR_AUDIO_STOP);
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
    msg_recv(MSG_CMD_CAR_DATA_SUBSCRIBE, *list);

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

    SEND_MESSAGE(MSG_CMD_CAR_DATA_SUBSCRIBE_DONE,
                 CCarLifeLib::getInstance()->cmdCarDataSubscribeDone(&reply), reply);
}

static void cb_car_data_subscribe_start(S_VEHICLE_INFO_LIST* list)
{
    msg_recv(MSG_CMD_CAR_DATA_SUBSCRIBE_START, *list);

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
    msg_recv(MSG_CMD_CAR_DATA_SUBSCRIBE_STOP, *list);

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

        /* The message goes out once a second, so a failure that persists is written once
         * per run of failures instead of once per second. */
        int rc;
        {
            CmdWriteLock lock;
            rc = lib->cmdCarGPS(&gps);
        }
        if (rc >= 0) {
            failure_logged = false;
            msg_send(MSG_CMD_CAR_GPS, rc, gps);
        } else if (!failure_logged) {
            msg_send(MSG_CMD_CAR_GPS, rc, gps);
            failure_logged = true;
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
    default: return "UNKNOWN";
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
            SEND_MESSAGE(MSG_CMD_HU_PROTOCOL_VERSION, lib->cmdHUProtoclVersion(&version), version);

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
            SEND_MESSAGE(MSG_CMD_VIDEO_ENCODER_INIT, lib->cmdVideoEncoderInit(&param), param);
            break;
        }
        case CMD_VIDEO_ENCODER_START:
            SEND_MESSAGE(MSG_CMD_VIDEO_ENCODER_START, lib->cmdVideoEncoderStart());
            break;
        case CMD_VIDEO_ENCODER_PAUSE:
            SEND_MESSAGE(MSG_CMD_VIDEO_ENCODER_PAUSE, lib->cmdVideoEncoderPause());
            break;
        case CMD_VIDEO_ENCODER_RESET:
            SEND_MESSAGE(MSG_CMD_VIDEO_ENCODER_RESET, lib->cmdVideoEncoderReset());
            break;
        case CMD_GO_TO_FOREGROUND:
            SEND_MESSAGE(MSG_CMD_GO_TO_FOREGROUND, lib->cmdGoToForeground());
            break;
        case CMD_LAUNCH_MODE_NORMAL:
            SEND_MESSAGE(MSG_CMD_LAUNCH_MODE_NORMAL, lib->cmdLaunchModeNormal());
            break;
        case CMD_LAUNCH_MODE_PHONE:
            SEND_MESSAGE(MSG_CMD_LAUNCH_MODE_PHONE, lib->cmdLaunchModePhone());
            break;
        case CMD_LAUNCH_MODE_MAP:
            SEND_MESSAGE(MSG_CMD_LAUNCH_MODE_MAP, lib->cmdLaunchModeMap());
            break;
        case CMD_LAUNCH_MODE_MUSIC:
            SEND_MESSAGE(MSG_CMD_LAUNCH_MODE_MUSIC, lib->cmdLaunchModeMusic());
            break;
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

    /* Pointer input goes out on the control channel, which has its own writer and does not
     * take the command channel lock, so these sends are not reported by SEND_MESSAGE. */
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
            msg_send(MSG_TOUCH_ACTION, lib->ctrlTouchAction(&action), action);
            break;
        }
        case CMD_TOUCH_DOWN: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_DOWN point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_ACTION_DOWN, lib->ctrlTouchActionDown(&point), point);
            break;
        }
        case CMD_TOUCH_UP: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_UP point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_ACTION_UP, lib->ctrlTouchActionUp(&point), point);
            break;
        }
        case CMD_TOUCH_MOVE: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_ACTION_MOVE point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_ACTION_MOVE, lib->ctrlTouchActionMove(&point), point);
            break;
        }
        case CMD_TOUCH_SINGLE_CLICK: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_SIGNAL_CLICK point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_SINGLE_CLICK, lib->ctrlTouchSigleClick(&point), point);
            break;
        }
        case CMD_TOUCH_DOUBLE_CLICK: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_DOUBLE_CLICK point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_DOUBLE_CLICK, lib->ctrlTouchDoubleClick(&point), point);
            break;
        }
        case CMD_TOUCH_LONG_PRESS: {
            if (body_length < 8) {
                return;
            }
            S_TOUCH_LONG_PRESS point;
            point.x = get_be32(body);
            point.y = get_be32(body + 4);
            msg_send(MSG_TOUCH_LONG_PRESS, lib->ctrlTouchLongPress(&point), point);
            break;
        }
        case CMD_HARD_KEY: {
            if (body_length < 4) {
                return;
            }
            S_TOUCH_CAR_HARD_KEY_CODE key;
            key.keycode = get_be32(body);
            msg_send(MSG_TOUCH_CAR_HARD_KEY_CODE, lib->ctrlTouchCarHardKeyCode(&key), key);
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
