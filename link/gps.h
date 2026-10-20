/*
 * Coarse position for the vehicle data channel, already scaled to the units of
 * MSG_CMD_CAR_GPS (guide 12.1.1.2).
 *
 * The value comes from CoreLocation, which on a Mac without a GNSS receiver resolves
 * Wi-Fi access points: the accuracy is tens to hundreds of metres. See gps.mm.
 */
#ifndef CARLIFE_GPS_H
#define CARLIFE_GPS_H

#include <stdint.h>

typedef struct {
    int valid;               /* 1 once a fix has arrived */
    int32_t latitude;        /* 1/1,000,000 degree, WGS84 */
    int32_t longitude;       /* 1/1,000,000 degree, WGS84 */
    int32_t height;          /* 0.1 m */
    uint32_t speed;          /* 0.01 km/h */
    uint32_t heading;        /* 0.1 degree */
    uint32_t hor_pos_error;  /* metres */
    uint32_t vert_pos_error; /* metres */
    uint64_t timestamp;      /* ms since the epoch */
    /* Calendar breakdown of timestamp, UTC */
    uint32_t year;
    uint32_t month;          /* 1..12 */
    uint32_t day;            /* 1..31 */
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} gps_fix;

/* Whether location services may be used: 1 allowed, 0 denied or restricted, -1 the
 * system prompt has not been answered yet. */
int gps_authorization(void);

/* Starts sampling on a thread of its own. Call once; there is no stop. Returns -1 when
 * the thread could not be created, in which case no position will ever be available. */
int gps_start(void);

/* Copies the latest fix. Returns 0 while none has arrived. */
int gps_latest(gps_fix* out);

#endif /* CARLIFE_GPS_H */
