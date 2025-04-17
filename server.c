#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <io.h>           // for _close, if needed
#include <sys/types.h>

// Windows-specific headers
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>      // for Sleep(...)
#pragma comment(lib, "ws2_32.lib")

#include <stdbool.h>

// If your Windows environment doesn't have gettimeofday, we can implement a quick replacement:
static int gettimeofday(struct timeval* tp, void* tzp) {
    // This is a rough implementation using WinAPI
    // Because PA1 doesn't demand microsecond precision, this might be acceptable.
    FILETIME ft;
    unsigned long long tmpres = 0;
    static const unsigned long long EPOCH = ((unsigned long long)116444736000000000ULL);

    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    // Convert from 100-nanosecond intervals to microseconds
    tmpres -= EPOCH;
    tmpres /= 10;

    tp->tv_sec = (long)(tmpres / 1000000UL);
    tp->tv_usec = (long)(tmpres % 1000000UL);

    return 0;
}

long time_diff_millis(struct timeval start, struct timeval end) {
    long sec = end.tv_sec - start.tv_sec;
    long usec = end.tv_usec - start.tv_usec;
    return (sec * 1000) + (usec / 1000);
}

// main entry
int main(int argc, char* argv[]) {
    return run_server(argc, argv);
}

int run_server(int argc, char* argv[])
{
    // Expected 8 args:
    //  [0] = program name
    //  1) chan_ip
    //  2) chan_port
    //  3) file_name
    //  4) frame_size
    //  5) slot_time
    //  6) seed
    //  7) timeout
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <chan_ip> <chan_port> <file> <frame_size> <slot_time> <seed> <timeout>\n", argv[0]);
        return 1;
    }

    char* chan_ip = argv[1];
    int   chan_port = atoi(argv[2]);
    char* file_name = argv[3];
    int   frame_size = atoi(argv[4]);
    int   slot_time = atoi(argv[5]);
    int   seed = atoi(argv[6]);
    int   timeout_sec = atoi(argv[7]);

    srand(seed);

    // Initialize Winsock
    WSADATA wsaData;
    int wsaerr = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaerr != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", wsaerr);
        return 1;
    }

    // Open file
    FILE* fp = fopen(file_name, "rb");
    if (!fp) {
        perror("fopen");
        WSACleanup();
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Create socket
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        fclose(fp);
        WSACleanup();
        return 1;
    }

    // Connect
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(chan_port);

    // If you have a newer Windows SDK, you can use InetPton:
    if (InetPton(AF_INET, chan_ip, &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "InetPton failed or invalid IP.\n");
        closesocket(sockfd);
        fclose(fp);
        WSACleanup();
        return 1;
    }

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect() failed: %d\n", WSAGetLastError());
        closesocket(sockfd);
        fclose(fp);
        WSACleanup();
        return 1;
    }

    //TODO: do we need it?
    int header_size = sizeof(FrameHeader);
    int payload_size = frame_size - header_size;
    if (payload_size <= 0) {
        fprintf(stderr, "Error: frame_size too small for header.\n");
        closesocket(sockfd);
        fclose(fp);
        WSACleanup();
        return 1;
    }
    //TODO: ??? why ???
    int total_frames = (int)((file_size + payload_size - 1) / payload_size);

    // Stats
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    int   max_trans_for_frame = 0;
    long  total_transmissions = 0;
    int   total_collisions = 0;
    bool  success_overall = true;

    for (int frame_i = 0; frame_i < total_frames; frame_i++) {
        long remaining = file_size - (frame_i * payload_size);
        int bytes_to_read = (int)((remaining < payload_size) ? remaining : payload_size);

        char* frame_buf = (char*)malloc(frame_size);
        memset(frame_buf, 0, frame_size);

        FrameHeader* hdr = (FrameHeader*)frame_buf;
        hdr->frame_type = FRAME_TYPE_DATA;
        hdr->length = (unsigned short)bytes_to_read;

        size_t read_ok = fread(frame_buf + header_size, 1, bytes_to_read, fp);
        if (read_ok < (size_t)bytes_to_read) {
            fprintf(stderr, "Error reading file.\n");
            free(frame_buf);
            success_overall = false;
            break;
        }

        int attempts_for_this_frame = 0;
        bool frame_sent_successfully = false;

        while (!frame_sent_successfully) {
            attempts_for_this_frame++;
            total_transmissions++;

            int sent = send(sockfd, frame_buf, frame_size, 0);
            if (sent == SOCKET_ERROR) {
                fprintf(stderr, "send() failed: %d\n", WSAGetLastError());
                success_overall = false;
                break;
            }

            // check overall timeout
            gettimeofday(&end_time, NULL);
            long elapsed_sec = time_diff_millis(start_time, end_time) / 1000;
            if (elapsed_sec >= timeout_sec) {
                success_overall = false;
                break;
            }

            // Wait for response (ACK or COLLISION)
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = slot_time * 2000; // e.g. 2x slot_time

            int rv = select((int)(sockfd + 1), &readfds, NULL, NULL, &tv);
            if (rv == SOCKET_ERROR) {
                fprintf(stderr, "select error: %d\n", WSAGetLastError());
                success_overall = false;
                break;
            }
            else if (rv == 0) {
                // no response => collision
                total_collisions++;
            }
            else {
                // read
                char ack_buf[256];
                memset(ack_buf, 0, sizeof(ack_buf));
                int rcv = recv(sockfd, ack_buf, sizeof(ack_buf), 0);
                if (rcv <= 0) {
                    // channel closed?
                    success_overall = false;
                    break;
                }
                FrameHeader* ack_hdr = (FrameHeader*)ack_buf;
                if (ack_hdr->frame_type == FRAME_TYPE_COLLISION) {
                    total_collisions++;
                }
                else if (ack_hdr->frame_type == FRAME_TYPE_ACK) {
                    frame_sent_successfully = true;
                }
                else {
                    // treat unknown as collision
                    total_collisions++;
                }
            }

            if (!frame_sent_successfully) {
                if (attempts_for_this_frame >= MAX_BACKOFF_ATTEMPTS) {
                    success_overall = false;
                    break;
                }
                // exponential backoff
                int k = attempts_for_this_frame;
                if (k > MAX_BACKOFF_ATTEMPTS) k = MAX_BACKOFF_ATTEMPTS;
                int R = rand() % (1 << k);
                // we used usleep(R * slot_time * 1000) in POSIX
                // On Windows, Sleep is in ms:
                Sleep(R * slot_time);
            }

            if (!success_overall) {
                break;
            }
        }

        free(frame_buf);
        if (attempts_for_this_frame > max_trans_for_frame) {
            max_trans_for_frame = attempts_for_this_frame;
        }
        if (!success_overall) {
            break;
        }
    }

    gettimeofday(&end_time, NULL);
    long total_msec = time_diff_millis(start_time, end_time);

    // Print stats
    fprintf(stderr, "Sent file %s\n", file_name);
    if (success_overall) {
        fprintf(stderr, "Result: Success :)\n");
    }
    else {
        fprintf(stderr, "Result: Failure :(\n");
    }
    fprintf(stderr, "File size: %ld Bytes (%d frames)\n", file_size, total_frames);
    fprintf(stderr, "Total transfer time: %ld milliseconds\n", total_msec);
    double avg_trans_per_frame = 0.0;
    if (total_frames > 0) {
        avg_trans_per_frame = ((double)total_transmissions + (double)total_collisions) / (double)total_frames;
    }
    fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n",
        avg_trans_per_frame, max_trans_for_frame);

    double secs = (double)total_msec / 1000.0;
    double mbps = 0.0;
    if (secs > 0.0) {
        mbps = ((double)file_size * 8.0) / (secs * 1000000.0);
    }
    fprintf(stderr, "Average bandwidth: %.3f Mbps\n", mbps);

    fclose(fp);
    closesocket(sockfd);
    WSACleanup();
    return 0;
}
