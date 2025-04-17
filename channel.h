#ifndef CHANNEL_H
#define CHANNEL_H

#include "server.h" // for FrameHeader, etc.
#include <winsock2.h>
#include <ws2tcpip.h>

// We'll track per-client stats
typedef struct {
    SOCKET sockfd;
    struct sockaddr_in addr;
    long frames_count;
    long collisions;
    long bytes_count;
} ClientInfo;

/*
 * run_channel()
 *   usage: run_channel(chan_port, slot_time)
 *   1) parse arguments
 *   2) accept multiple connections
 *   3) each slot_time, see if 1 frame => ACK, 2+ frames => collision
 *   4) on EOF, print stats
 */
int run_channel(int argc, char* argv[]);

#endif
