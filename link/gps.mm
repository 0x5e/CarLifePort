/*
 * Coarse position from CoreLocation, in the units of MSG_CMD_CAR_GPS.
 *
 * A Mac has no GNSS receiver: CLLocationManager resolves the position from Wi-Fi access
 * points, so a fix is tens to hundreds of metres off. That is the accuracy the vehicle
 * data channel is used with (guide 12), and it is the only source available here.
 *
 * Sampling runs from gps_start() until the process exits, whether or not the phone has
 * subscribed: the subscription reply has to state whether a position can be reported at
 * all, and that answer is only known after the authorization prompt has been answered.
 * Reporting is gated separately, in main.cpp.
 *
 * The manager has to live on a thread with a run loop, because that is where
 * CLLocationManager delivers its delegate callbacks; the main thread of this process is
 * inside the USB receive loop, so gps_start() creates one.
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "gps.h"

/* Latest fix and authorization state. Written on the run loop thread, read by the
 * reporting thread in main.cpp. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static gps_fix g_fix;
static int g_authorization = -1;

static CLLocationManager* g_manager;
static id g_delegate;

static int32_t clamp_i32(double value)
{
    if (value > 2147483647.0) {
        return 2147483647;
    }
    if (value < -2147483648.0) {
        return -2147483648;
    }
    return (int32_t)value;
}

static uint32_t clamp_u32(double value)
{
    if (value < 0.0) {
        return 0;
    }
    if (value > 4294967295.0) {
        return 4294967295;
    }
    return (uint32_t)value;
}

/* 1 authorized, 0 denied or restricted, -1 the question has not been answered yet. macOS
 * has one "authorized" state; the when-in-use one exists on iOS only. */
static int authorization_value(CLAuthorizationStatus status)
{
    if (status == kCLAuthorizationStatusAuthorizedAlways) {
        return 1;
    }
    if (status == kCLAuthorizationStatusNotDetermined) {
        return -1;
    }
    return 0;
}

static void set_authorization(CLAuthorizationStatus status)
{
    pthread_mutex_lock(&g_lock);
    g_authorization = authorization_value(status);
    pthread_mutex_unlock(&g_lock);
}

@interface CarLifeGps : NSObject <CLLocationManagerDelegate>
@end

@implementation CarLifeGps

- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager
{
    set_authorization(manager.authorizationStatus);
}

- (void)locationManager:(CLLocationManager*)manager didUpdateLocations:(NSArray*)locations
{
    CLLocation* location = [locations lastObject];
    if (location == nil) {
        return;
    }
    /* A negative accuracy marks a fix the system itself does not trust. */
    if (location.horizontalAccuracy < 0) {
        return;
    }

    gps_fix fix;
    memset(&fix, 0, sizeof(fix));
    fix.valid = 1;
    fix.latitude = clamp_i32(round(location.coordinate.latitude * 1000000.0));
    fix.longitude = clamp_i32(round(location.coordinate.longitude * 1000000.0));
    /* The altitude is meaningless when the vertical accuracy is negative, and the guide
     * has no way to say "unknown": the field is required, so it stays 0. */
    if (location.verticalAccuracy >= 0) {
        fix.height = clamp_i32(round(location.altitude * 10.0));
    }
    fix.speed = clamp_u32(round(location.speed * 360.0));   /* m/s -> 0.01 km/h */
    fix.heading = clamp_u32(round(location.course * 10.0)); /* -1 while the course is unknown */
    fix.hor_pos_error = clamp_u32(round(location.horizontalAccuracy));
    fix.vert_pos_error = clamp_u32(round(location.verticalAccuracy));
    fix.timestamp = (uint64_t)(location.timestamp.timeIntervalSince1970 * 1000.0);

    /* UTC, the same instant as timestamp: the field block is the date and time of the fix,
     * which is what a GPS receiver reports, and the guide does not name a time zone. */
    time_t seconds = (time_t)location.timestamp.timeIntervalSince1970;
    struct tm utc;
    if (gmtime_r(&seconds, &utc) != NULL) {
        fix.year = (uint32_t)(utc.tm_year + 1900);
        fix.month = (uint32_t)(utc.tm_mon + 1);
        fix.day = (uint32_t)utc.tm_mday;
        fix.hour = (uint32_t)utc.tm_hour;
        fix.minute = (uint32_t)utc.tm_min;
        fix.second = (uint32_t)utc.tm_sec;
    }

    pthread_mutex_lock(&g_lock);
    g_fix = fix;
    pthread_mutex_unlock(&g_lock);
}

@end

static void* gps_thread(void*)
{
    @autoreleasepool {
        g_delegate = [[CarLifeGps alloc] init];
        g_manager = [[CLLocationManager alloc] init];
        g_manager.delegate = g_delegate;
        /* Coarse on purpose: Wi-Fi positioning cannot do better than this, and a tighter
         * request only keeps the radio busy. */
        g_manager.desiredAccuracy = kCLLocationAccuracyHundredMeters;
        g_manager.distanceFilter = kCLDistanceFilterNone;

        /* The loop below waits in NSDefaultRunLoopMode, and a mode with no source in it
         * returns at once - with location services denied, CLLocationManager installs none,
         * and the loop then spins at one core. The port is never sent anything; it exists
         * so every iteration waits for the full second instead of returning immediately. */
        [[NSRunLoop currentRunLoop] addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode];

        /* The delegate is only called when the status changes, so the starting value has to
         * be read here; without this a denied process stays at -1 ("not answered") and the
         * subscription reply would claim a GPS that never delivers. */
        CLAuthorizationStatus status = g_manager.authorizationStatus;
        set_authorization(status);
        if (status == kCLAuthorizationStatusNotDetermined) {
            [g_manager requestWhenInUseAuthorization];
        }
        [g_manager startUpdatingLocation];
    }

    /* Sliced rather than CFRunLoopRun() so that each batch of callbacks is drained out of
     * the autorelease pool instead of accumulating for the lifetime of the process. */
    for (;;) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        }
    }
    return NULL;
}

int gps_authorization(void)
{
    int value;
    pthread_mutex_lock(&g_lock);
    value = g_authorization;
    pthread_mutex_unlock(&g_lock);
    return value;
}

int gps_start(void)
{
    static int started = 0;
    if (started) {
        return 0;
    }
    started = 1;

    pthread_t thread;
    if (pthread_create(&thread, NULL, gps_thread, NULL) != 0) {
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

int gps_latest(gps_fix* out)
{
    int valid;
    pthread_mutex_lock(&g_lock);
    *out = g_fix;
    valid = g_fix.valid;
    pthread_mutex_unlock(&g_lock);
    return valid;
}
