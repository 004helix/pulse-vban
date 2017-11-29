/*
 *  VBAN Receiver
 *
 *  Copyright (C) 2017 Raman Shyshniou <rommer@ibuffed.com>
 *  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <alloca.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "vban.h"
#include "streams.h"
#include "output.h"


#define STREAM_TIMEOUT_MSEC 700
#define BUFFER_OUT_PACKETS 2


/*
 * on connect/disconnect hooks
 */

static char *onconnect = NULL;
static char *ondisconnect = NULL;

static void error(char *msg)
{
    perror(msg);
    exit(1);
}

static void runhook(char *prog)
{
    char *argv[2];
    pid_t pid = fork();

    switch (pid) {
        case -1:
            fprintf(stderr, "exec failed, unable to fork: %s\n", strerror(errno));
            return;
        case 0:
            argv[0] = prog;
            argv[1] = NULL;
            execvp(prog, argv);
            fprintf(stderr, "execvp failed: %s\n", strerror(errno));
            exit(1);
        break;
    }
}


int syncstreams(struct stream *stream1, struct stream *stream2, int64_t *offset)
{
    int i, w, matches;
    long size = stream2->datasize;
    char *data1, data2[size * 2];

    // compare streams
    if (stream1->samples != stream2->samples ||
        stream1->datatype != stream2->datatype ||
        stream1->channels != stream2->channels ||
        stream1->sample_rate != stream2->sample_rate)
        return -1;

    // try to find last packet of stream1
    // in the last two packets of stream2

    if (!stream2->prev)
        // not enough consequitive packets in stream2
        return 0;

    data1 = stream1->curr;
    memcpy(data2, stream2->prev, size);
    memcpy(data2 + size, stream2->curr, size);

    matches = 0;
    w = stream1->sample_size * stream1->channels;
    for (i = 0; i < size; i += w)
        if (!memcmp(data1, data2 + i, size)) {
            if (matches == 0) {
                *offset = (int64_t) stream2->expected - 1;
                *offset -= (int64_t) stream1->expected;
                *offset *= stream1->samples;
                *offset += i / w;
            }
            matches++;
        }

    return matches;
}


void run(int sock, int pipefd)
{
    struct stream *stream, *dead;

    while (1) {
        stream = recvvban(sock);

        // all streams disconnected or fatal error
        if (!stream) {
            if (streams) {
                forgetstreams();
                if (ondisconnect)
                    runhook(ondisconnect);
            }
            return;
        }

        // check dead streams
        for (dead = streams; dead; dead = dead->next) {
            long msec;

            // skip current stream
            if (dead == stream)
                continue;

            msec = (long) (stream->tv.tv_sec - dead->tv.tv_sec) * 1000L;
            msec += (long) (stream->tv.tv_usec - dead->tv.tv_usec) / 1000L;

            if (msec < STREAM_TIMEOUT_MSEC)
                continue;

            if (dead == streams) {
                // primary stream died
                // fix offsets and out position
                struct stream *curr;
                int64_t delta;

                if (!streams->next) {
                    if (ondisconnect)
                        runhook(ondisconnect);
                    forgetstreams();
                    return;
                }

                delta = dead->next->offset;

                for (curr = dead->next; curr; curr = curr->next)
                    curr->offset -= delta;

                output_move(delta);
            }

            forgetstream(dead);
        }

        // ignore stream
        if (stream->ignore)
            continue;

        // stream sync paused
        if (stream->insync < 0) {
            stream->insync++;
            continue;
        }

        // require 3 successfully attempts to sync
        if (stream->insync < 3) {
            struct stream *primary = streams;
            int64_t offset;
            int matches;

            if (stream == primary) {
                fprintf(stderr, "[%s] stream online, primary\n",
                        stream->name);

                output_init(stream->samples * BUFFER_OUT_PACKETS);
                if (onconnect)
                    runhook(onconnect);

                stream->insync = 3;
                continue;
            }

            matches = syncstreams(primary, stream, &offset);

            if (matches < 0) {
                fprintf(stderr, "[%s] stream didnt match primary stream\n",
                        stream->name);
                stream->ignore++;
                continue;
            }

            if (matches == 0) {
                matches = syncstreams(stream, primary, &offset);
                offset = -offset;
            }

            if (matches == 1) {
                if (stream->insync++ && stream->offset != offset) {
                    // offset mismatch, pause (250ms) and try to sync again
                    stream->insync = -(long) (stream->sample_rate / stream->samples / 4);
                    continue;
                }

                if (stream->insync == 3)
                    fprintf(stderr, "[%s] stream online, offset %lld samples\n",
                            stream->name, (long long) offset);

                stream->offset = offset;
            } else {
                if (stream->insync == 0)
                    // still cant sync, pause (250ms) stream for a while
                    stream->insync = -(long) (stream->sample_rate / stream->samples / 4);
            }

            continue;
        }

        // play data
        output_play(pipefd,
                    stream->expected * stream->samples - stream->offset,
                    stream->samples, stream->curr, stream->datasize);
    }
}


int main(int argc, char **argv)
{
    int port, sock, pipefd, optval = 1;
    struct sockaddr_in addr;
    struct timeval timeout;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    // check command line arguments
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <pipe> [exec-on-connect] [exec-on-disconnect]\n", argv[0]);
        return 1;
    }

    // parse port
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port: %s\n", argv[1]);
        return 1;
    }

    // create listen socket
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        error("socket");

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&optval , sizeof(optval));

    // server's address to listen on
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    // bind
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        error("bind");

    // set receive timeout
    timeout.tv_sec = STREAM_TIMEOUT_MSEC / 1000000;
    timeout.tv_usec = (STREAM_TIMEOUT_MSEC % 100000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
                   sizeof(timeout)) < 0)
        error("setsockopt failed");

    // set SO_TIMESTAMP
    optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMP, &optval,
                   sizeof(optval)) < 0)
        error("setsockopt failed");

    // open pipe
    pipefd = open(argv[2], O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (pipefd < 0)
        error("pipe open");

    // setup connect/disconnect handlers
    if (argc > 3)
        onconnect = strdup(argv[3]);
    if (argc > 4)
        ondisconnect = strdup(argv[4]);

    // run
    while (1) {
        run(sock, pipefd);
    }

    return 0;
}
