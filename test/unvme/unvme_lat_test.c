/**
 * Copyright (c) 2015-2016, Micron Technology, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief UNVMe I/O latency test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>

#include "libunvme.h"
#include "rdtsc.h"

/// macro to print an error message
#define ERROR(fmt, arg...)  error(1, 0, "ERROR: " fmt "\n", ##arg)

/// macro to print an io related error message
#define IOERROR(s, p)       ERROR(s " buf=%p lba=%#lx", (p)->buf, (p)->lba)

/// page structure
typedef struct {
    void*           buf;        ///< IO buffer
    u64             lba;        ///< lba
    unvme_iod_t     iod;        ///< returned IO descriptor
    u64             tsc;        ///< tsc time
} lat_page_t;

// Global variables
static const unvme_ns_t* ns;    ///< unvme namespace pointer
static int nsid = 1;            ///< namespace id
static int qcount = 1;          ///< queue count
static int qsize = 8;           ///< queue size
static int runtime = 30;        ///< run time in seconds
static u64 endtsc = 0;          ///< end run tsc
static sem_t sem1;              ///< semaphore to start thread
static sem_t sem2;              ///< semaphore to start test
static pthread_t* ses;          ///< array of thread sessions
static u64 last_lba;            ///< last page boundary lba
static u64 ioc;                 ///< total number of io count
static u64 avg_slat;            ///< total submission time
static u64 avg_clat;            ///< total completion time
static u64 min_slat;            ///< minimum submission time
static u64 max_slat;            ///< maximum submission time
static u64 min_clat;            ///< minimum completimesn time
static u64 max_clat;            ///< maximum completimesn time

/**
 * Submit an io and record the submission latency time.
 */
static void io_submit(int q, int rw, lat_page_t* p)
{
    // change lba
    p->lba += ns->nbpp << 1;
    if (p->lba > last_lba) p->lba &= last_lba;

    p->tsc = rdtsc();
    if (rw) {
        p->iod = unvme_awrite(ns, q, p->buf, p->lba, ns->nbpp);
        if (!p->iod) IOERROR("awrite", p);
    } else {
        p ->iod = unvme_aread(ns, q, p->buf, p->lba, ns->nbpp);
        if (!p->iod) IOERROR("aread", p);
    }
    ioc++;

    u64 ts = rdtsc_elapse((u64)(p->tsc));
    if (min_slat > ts) min_slat = ts;
    if (max_slat < ts) max_slat = ts;
    avg_slat += ts;
}

/**
 * Queue thread test.
 */
static void* run_thread(void* arg)
{
    int rw = (long)arg >> 16;
    int q = (long)arg & 0xffff;

    u64 lba = (q * qcount * qsize * ns->nbpp) << 1;
    lat_page_t* pages = calloc(ns->maxiopq, sizeof(lat_page_t));
    lat_page_t* p = pages;
    int i;
    for (i = 0; i < ns->maxiopq; i++) {
        p->buf = unvme_alloc(ns, ns->pagesize);
        lba += (ns->nbpp << 1);
        if (lba > last_lba) lba = i * ns->nbpp;
        p->lba = lba;
        p++;
    }

    sem_post(&sem1);
    sem_wait(&sem2);

    for (i = 0; i < ns->maxiopq; i++) io_submit(q, rw, pages + i);

    i = 0;
    int pending = ns->maxiopq;
    do {
        p = pages + i;
        if (p->iod != 0 && unvme_apoll(p->iod, 0) == 0) {
            u64 tc = rdtsc_elapse(p->tsc);
            if (min_clat > tc) min_clat = tc;
            if (max_clat < tc) max_clat = tc;
            avg_clat += tc;
        
            if ((tc + p->tsc) < endtsc) {
                io_submit(q, rw, p);
            } else {
                p->iod = 0;
                pending--;
            }
        }
        if (++i == ns->maxiopq) i = 0;
    } while (pending > 0);

    p = pages;
    for (i = 0; i < ns->maxiopq; i++) {
        unvme_free(ns, p->buf);
        p++;
    }
    free(pages);
    return 0;
}


/**
 * Run test to spawn one thread for each queue.
 */
void run_test(const char* name, int rw)
{
    ioc = 0;
    avg_slat = 0;
    avg_clat = 0;
    min_slat = -1;
    max_slat = 0;
    min_clat = -1;
    max_clat = 0;

    int q;
    for (q = 0; q < qcount; q++) {
        long arg = (rw << 16) + q;
        pthread_create(&ses[q], 0, run_thread, (void*)arg);
        sem_wait(&sem1);
    }

    sleep(1);
    time_t te = time(0);
    struct tm* t = localtime(&te);
    printf("%s: run test for %d seconds (%02d:%02d:%02d)\n",
           name, runtime, t->tm_hour, t->tm_min, t->tm_sec);
    endtsc = rdtsc() + (runtime * rdtsc_second());

    for (q = 0; q < qcount; q++) sem_post(&sem2);
    for (q = 0; q < qcount; q++) pthread_join(ses[q], 0);

    /*
    printf("%s: slat=(%lu %lu %lu) lat=(%lu %lu %lu) ioc=%ld\n",
            name, min_slat, max_slat, avg_slat/ioc,
            min_clat, max_clat, avg_clat/ioc, ioc);
    */
    u64 utsc = rdtsc_second() / 1000000;
    printf("%s: slat=(%.2f %.2f %.2f) lat=(%.2f %.2f %.2f) usecs ioc=%ld\n",
            name, (double)min_slat/utsc, (double)max_slat/utsc,
            (double)avg_slat/ioc/utsc, (double)min_clat/utsc,
            (double)max_clat/utsc, (double)avg_clat/ioc/utsc, ioc);
}

/**
 * Main program.
 */
int main(int argc, char* argv[])
{
    const char* usage =
"Usage: %s [OPTION]... pciname\n\
         -n       nsid (default to 1)\n\
         -q       queue count (default 1)\n\
         -d       queue depth (default 8)\n\
         -t       run time in seconds (default 30)\n\
         pciname  PCI device name (as BB:DD.F) format\n";

    char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    int opt;
    while ((opt = getopt(argc, argv, "n:q:d:t:")) != -1) {
        switch (opt) {
        case 'n':
            nsid = atoi(optarg);
            if (nsid <= 0) error(1, 0, "n must be > 0");
            break;
        case 'q':
            qcount = atoi(optarg);
            if (qcount <= 0) error(1, 0, "q must be > 0");
            break;
        case 'd':
            qsize = atoi(optarg);
            if (qsize <= 1) error(1, 0, "d must be > 1");
            break;
        case 't':
            runtime = atoi(optarg);
            if (runtime <= 0) error(1, 0, "r must be > 0");
            break;
        default:
            error(1, 0, usage, prog);
        }
    }
    if (optind >= argc) error(1, 0, usage, prog);
    char* pciname = argv[optind];

    printf("LATENCY TEST BEGIN\n");
    ns = unvme_open(pciname, nsid, qcount, qsize);
    if (!ns) ERROR("open %s failed", pciname);
    last_lba = (ns->blockcount - ns->nbpp) & ~(u64)(ns->nbpp - 1);
    printf("nsid=%d qc=%d qd=%d cap=%ld mbio=%d lastlba=%#lx\n",
            nsid, qcount, qsize, ns->blockcount, ns->maxbpio, last_lba);

    sem_init(&sem1, 0, 0);
    sem_init(&sem2, 0, 0);
    ses = calloc(qcount, sizeof(pthread_t));

    run_test("read", 0);
    run_test("write", 0);

    free(ses);
    sem_destroy(&sem1);
    sem_destroy(&sem2);
    unvme_close(ns);
    printf("LATENCY TEST COMPLETE\n");

    return 0;
}

