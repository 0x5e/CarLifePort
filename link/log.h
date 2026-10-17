/*
 * log.h — the one lock around stderr in the link process.
 *
 * The transport half (aoa.c), the protocol stack half (main.cpp) and the six channel threads
 * all write to stderr, and each line is written in several calls. Without a shared lock a
 * line is split by another thread's line, and the Go backend, which relays stderr line by
 * line, reads the two as one message with the level and module of the first.
 *
 * Every writer holds the lock around a whole line, newline included.
 */
#ifndef CARLIFE_LINK_LOG_H
#define CARLIFE_LINK_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void aoa_log_lock(void);
void aoa_log_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
