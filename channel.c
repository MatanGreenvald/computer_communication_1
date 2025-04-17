#include "channel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <stdbool.h>
#include <conio.h>
#pragma comment(lib, "ws2_32.lib")

#ifndef _WIN32
#include <unistd.h>
#endif

#define MAX_CLIENTS 100
#define FRAME_MAX   2000 // arbitrary maximum frame size

int run_channel(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <chan_port> <slot_time_ms>\n", argv[0]);
        return 1;
    }

    int chan_port = atoi(argv[1]);
    int slot_time = atoi(argv[2]); // ms

    WSADATA wsaData;
    int wsaerr = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaerr != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", wsaerr);
        return 1;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int optval = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons((u_short)chan_port);

    if (bind(listen_sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    if (listen(listen_sock, 10) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    ClientInfo clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));
    int num_clients = 0;

    fprintf(stderr, "Channel: listening on port %d, slot_time=%d ms\n", chan_port, slot_time);
    fprintf(stderr, "(Press Ctrl+Z followed by <Enter> to quit)\n");

    bool running = true;
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        SOCKET maxSock = listen_sock;

        for (int i = 0; i < num_clients; i++) {
            SOCKET s = clients[i].sockfd;
            if (s != INVALID_SOCKET) {
                FD_SET(s, &readfds);
                if (s > maxSock) maxSock = s;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = slot_time * 1000;

        int ready = select((int)(maxSock + 1), &readfds, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            fprintf(stderr, "select error: %d\n", err);
            break;
        }

        if (FD_ISSET(listen_sock, &readfds)) {
            struct sockaddr_in caddr;
            int caddr_len = sizeof(caddr);
            SOCKET new_sock = accept(listen_sock, (struct sockaddr*)&caddr, &caddr_len);
            if (new_sock == INVALID_SOCKET) {
                fprintf(stderr, "accept failed: %d\n", WSAGetLastError());
            }
            else {
                if (num_clients < MAX_CLIENTS) {
                    clients[num_clients].sockfd = new_sock;
                    clients[num_clients].addr = caddr;
                    clients[num_clients].frames_count = 0;
                    clients[num_clients].collisions = 0;
                    clients[num_clients].bytes_count = 0;
                    num_clients++;
                    fprintf(stderr, "Channel: new client connected!\n");
                }
                else {
                    fprintf(stderr, "Channel: too many clients!\n");
                    closesocket(new_sock);
                }
            }
        }

        int frames_in_this_slot = 0;
        int indices[MAX_CLIENTS];
        char buffers[MAX_CLIENTS][FRAME_MAX];
        int lengths[MAX_CLIENTS];

        for (int i = 0; i < num_clients; i++) {
            SOCKET s = clients[i].sockfd;
            if (s != INVALID_SOCKET && FD_ISSET(s, &readfds)) {
                int rcv = recv(s, buffers[frames_in_this_slot], FRAME_MAX, 0);
                if (rcv <= 0) {
                    closesocket(s);
                    clients[i].sockfd = INVALID_SOCKET;
                }
                else {
                    indices[frames_in_this_slot] = i;
                    lengths[frames_in_this_slot] = rcv;
                    frames_in_this_slot++;
                }
            }
        }

        if (frames_in_this_slot == 1) {
            int idx = indices[0];
            clients[idx].frames_count++;
            clients[idx].bytes_count += lengths[0];

            FrameHeader* hdr = (FrameHeader*)buffers[0];
            if (hdr->frame_type == FRAME_TYPE_DATA) {
                FrameHeader ack;
                ack.frame_type = FRAME_TYPE_ACK;
                ack.length = 0;
                send(clients[idx].sockfd, (char*)&ack, sizeof(ack), 0);
            }
        }
        else if (frames_in_this_slot > 1) {
            for (int k = 0; k < frames_in_this_slot; k++) {
                int idx = indices[k];
                clients[idx].collisions++;
                FrameHeader col;
                col.frame_type = FRAME_TYPE_COLLISION;
                col.length = 0;
                send(clients[idx].sockfd, (char*)&col, sizeof(col), 0);
            }
        }

#ifdef _WIN32
        while (_kbhit()) {
            int ch = _getch() & 0xFF;
            if (ch == 0x1A || ch == 0x04) {
                fprintf(stderr, "\nChannel: EOF detected! Shutting down…\n");
                running = false;
            }
        }
#else
        if (feof(stdin)) {
            fprintf(stderr, "\nChannel: EOF detected! Shutting down…\n");
            running = false;
        }
#endif
    }

    for (int i = 0; i < num_clients; i++) {
        SOCKET s = clients[i].sockfd;
        if (s != INVALID_SOCKET) {
            struct sockaddr_in* a = &clients[i].addr;
            char ip_str[INET_ADDRSTRLEN];
            if (InetNtop(AF_INET, &a->sin_addr, ip_str, sizeof(ip_str)) == NULL) {
                strcpy(ip_str, "unknown");
            }
            int cport = ntohs(a->sin_port);
            fprintf(stderr, "From %s port %d: %ld frames, %ld collisions\n", ip_str, cport, clients[i].frames_count, clients[i].collisions);
            closesocket(s);
        }
    }

    closesocket(listen_sock);
    WSACleanup();
    fprintf(stderr, "Channel: done.\n");
    return 0;
}

int main(int argc, char* argv[]) {
    return run_channel(argc, argv);
}
