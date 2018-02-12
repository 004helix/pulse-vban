/*
 *  VBAN Receiver
 *
 *  Copyright (C) 2017, 2018 Raman Shyshniou <rommer@ibuffed.com>
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

#ifndef _HTTPD_H
#define _HTTPD_H 1

#include <stdint.h>
#include <net/if.h>
#include "streams.h"

// stream snapshot
struct stream_snap {
    // remote address
    struct sockaddr_storage peer;

    // receiving interface name
    char ifname[IF_NAMESIZE];

    // stream name
    char name[20];

    // data format
    char *format;              // sample format name
    long sample_rate;          // sample rate
    long channels;             // channels

    // stream counters
    long lost;                 // total lost packets counter
    uint32_t expected;         // next expected packet number in this stream
    struct timespec ts_first;  // first packet received time
    struct timespec ts_last;   // last packet received time
    double dt_average;         // average nanoseconds between packets, EWMA
    double dt_variance;        // average variance between packets, EWMV

    // synchronization
    long ignore;               // ignore this stream
    long insync;               // synchronized with primary stream
    int64_t offset;            // stream offset
};

struct snapshot_cell {
    struct stream_snap *ss;
    int ss_size;
    int count;
    long lost;
};

void httpd_update(struct stream *streams);
int httpd(int sock);

#endif
