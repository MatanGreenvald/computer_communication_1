#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <winsock2.h>    // For struct timeval on Windows, though not standard
#include <ws2tcpip.h>    // Possibly for InetPton, etc.

/*
 * Frame type definitions for our assignment
 */
#define FRAME_TYPE_DATA      1
#define FRAME_TYPE_COLLISION 2
#define FRAME_TYPE_ACK       3

#define MAX_BACKOFF_ATTEMPTS 10

#pragma pack(push, 1)
typedef struct {
    unsigned short frame_type; // e.g. FRAME_TYPE_DATA
    unsigned short length;     // payload size
    // Add fields if needed (seq #, etc.)
} FrameHeader;
#pragma pack(pop)

/*
 * The main server logic function:
 *  usage: run_server(chan_ip, chan_port, file_name, frame_size, slot_time, seed, timeout)
 */
int run_server(int argc, char* argv[]);

/*
 * Helper for time difference in milliseconds.
 * On Windows, we'll implement it using gettimeofday-like structures
 * or custom code. We might do a fallback if needed.
 */
long time_diff_millis(struct timeval start, struct timeval end);

#endif
