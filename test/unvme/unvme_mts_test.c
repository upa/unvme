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
 * @brief UNVMe multi-threaded/session test.
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

#include "unvme.h"

// Global variables
static char* pciname;           ///< PCI device name
static int nsid = 1;            ///< namespace id
static int numses = 4;          ///< number of thread sessions
static int qcount = 6;          ///< number of queues per session to create
static int qsize = 100;         ///< queue size
static int maxnlb = 2048;       ///< maximum number of blocks per IO
static sem_t sm_ready;          ///< semaphore for ready
static sem_t sm_start;          ///< semaphore for start

/// thread session arguments
typedef struct {
    const unvme_ns_t*   ns;     ///< namespace handle
    int                 id;     ///< session id
    int                 qid;    ///< queue id
    u64                 slba;   ///< starting lba
} ses_arg_t;


/**
 * Thread per queue.
 */
void* test_queue(void* arg)
{
    ses_arg_t* ses = arg;
    u64 slba, wlen, w, *p;
    int nlb, l, i;

    sem_post(&sm_ready);
    sem_wait(&sm_start);

    printf("Test s%d q%d started (lba %#lx)\n", ses->id, ses->qid, ses->slba);
    unvme_iod_t* iod = calloc(qsize, sizeof(unvme_iod_t));
    void** buf = calloc(qsize, sizeof(void*));
    int* buflen = calloc(qsize, sizeof(int));

    for (l = 0; l < numses; l++) {
        // allocate buffers
        for (i = 0; i < qsize; i++) {
            nlb = rand() % maxnlb + 1;
            buflen[i] = nlb * ses->ns->blocksize;
            if (!(buf[i] = unvme_alloc(ses->ns, buflen[i])))
                error(1, 0, "alloc.%d.%d.%d failed", ses->id, ses->qid, i);
        }

#ifdef DO_SYNC_WRITE_READ
        slba = ses->slba;
        for (i = 0; i < qsize; i++) {
            nlb = buflen[i] / ses->ns->blocksize;
            wlen = buflen[i] / sizeof (u64);
            p = buf[i];
            for (w = 0; w < wlen; w++) p[w] = (w << 32) + i;
            if (unvme_write(ses->ns, ses->qid, p, slba, nlb))
                error(1, 0, "write.%d.%d.%d failed", ses->id, ses->qid, i);
            bzero(p, buflen[i]);
            if (unvme_read(ses->ns, ses->qid, p, slba, nlb))
                error(1, 0, "read.%d.%d.%d failed", ses->id, ses->qid, i);
            for (w = 0; w < wlen; w++) {
                if (p[w] != ((w << 32) + i))
                    error(1, 0, "data.%d.%d.%d error", ses->id, ses->qid, i);
            }
            slba += nlb;
        }
#else
        // async write
        slba = ses->slba;
        for (i = 0; i < qsize; i++) {
            nlb = buflen[i] / ses->ns->blocksize;
            wlen = buflen[i] / sizeof (u64);
            p = buf[i];
            for (w = 0; w < wlen; w++) p[w] = (w << 32) + i;
            if (!(iod[i] = unvme_awrite(ses->ns, ses->qid, p, slba, nlb)))
                error(1, 0, "awrite.%d.%d.%d failed", ses->id, ses->qid, i);
            slba += nlb;
        }

        // async poll to complete all writes
        for (i = 0; i < qsize; i++) {
            if (unvme_apoll(iod[i], UNVME_TIMEOUT))
                error(1, 0, "apoll.%d.%d.%d failed", ses->id, ses->qid, i);
        }

        // do sync read and compare
        slba = ses->slba;
        for (i = 0; i < qsize; i++) {
            nlb = buflen[i] / ses->ns->blocksize;
            wlen = buflen[i] / sizeof (u64);
            p = buf[i];
            bzero(p, buflen[i]);
            if (unvme_read(ses->ns, ses->qid, p, slba, nlb))
                error(1, 0, "read.%d.%d.%d failed", ses->id, ses->qid, i);
            for (w = 0; w < wlen; w++) {
                if (p[w] != ((w << 32) + i))
                    error(1, 0, "data.%d.%d.%d error", ses->id, ses->qid, i);
            }
            slba += nlb;
        }
#endif

        // free buffers
        for (i = 0; i < qsize; i++) {
            if (unvme_free(ses->ns, buf[i]))
                error(1, 0, "free failed");
        }
    }

    free(buflen);
    free(buf);
    free(iod);
    printf("Test s%d q%d completed (lba %#lx)\n", ses->id, ses->qid, ses->slba);

    return 0;
}

/**
 * Thread per session.
 */
void* test_session(void* arg)
{
    int sesid = (long)arg;
    int sid = sesid + 1;

    printf("Session %d started\n", sid);
    const unvme_ns_t* ns = unvme_open(pciname, nsid, qcount, qsize);
    if (!ns) error(1, 0, "unvme_open %d failed", sid);

    u64 bpq = ns->blockcount / numses / qcount;

    pthread_t* qt = calloc(qcount, sizeof(pthread_t));
    ses_arg_t* sarg = calloc(qcount, sizeof(ses_arg_t));

    int q;
    for (q = 0; q < qcount; q++) {
        sarg[q].ns = ns;
        sarg[q].id = sid;
        sarg[q].qid = q;
        sarg[q].slba = bpq * (sesid * qcount + q);
        pthread_create(&qt[q], 0, test_queue, &sarg[q]);
        sem_wait(&sm_ready);
    }
    for (q = 0; q < qcount; q++) sem_post(&sm_start);
    for (q = 0; q < qcount; q++) pthread_join(qt[q], 0);

    free(sarg);
    free(qt);
    unvme_close(ns);
    printf("Session %d completed\n", sid);

    return 0;
}

/**
 * Main program.
 */
int main(int argc, char* argv[])
{
    const char* usage =
"Usage: %s [OPTION]... pciname\n\
         -n       nsid (default to 1)\n\
         -t       number of sessions (default 4)\n\
         -q       number of queues per session (default 6)\n\
         -d       each queue size (default 100)\n\
         -m       maximum number of blocks per IO (default 2048)\n\
         pciname  PCI device name (as BB:DD.F format)\n";

    char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    int opt, i;
    while ((opt = getopt(argc, argv, "n:t:q:d:m:")) != -1) {
        switch (opt) {
        case 'n':
            nsid = atoi(optarg);
            if (nsid <= 0) error(1, 0, "n must be > 0");
            break;
        case 't':
            numses = atoi(optarg);
            if (numses <= 0) error(1, 0, "t must be > 0");
            break;
        case 'q':
            qcount = atoi(optarg);
            if (qcount <= 0) error(1, 0, "q must be > 0");
            break;
        case 'd':
            qsize = atoi(optarg);
            if (qsize <= 1) error(1, 0, "d must be > 1");
            break;
        case 'm':
            maxnlb = atoi(optarg);
            if (maxnlb <= 0) error(1, 0, "m must be > 0");
            break;
        default:
            error(1, 0, usage, prog);
        }
    }
    if (optind >= argc) error(1, 0, usage, prog);
    pciname = argv[optind];

    printf("MULTI-SESSION TEST BEGIN\n");
    const unvme_ns_t* ns = unvme_open(pciname, nsid, qcount, qsize);
    if (!ns) error(1, 0, "unvme_open failed");
    printf("nsid=%d ses=%d qc=%d qd=%d maxnlb=%d cap=%lx\n",
           nsid, numses, qcount, qsize, maxnlb, ns->blockcount);
    if ((u64)(numses * qcount * qsize * maxnlb) > ns->blockcount)
        error(1, 0, "not enough disk space");
    unvme_close(ns);

    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_start, 0, 0);
    srand(time(0));
    pthread_t* st = calloc(numses * qcount, sizeof(pthread_t));

    for (i = 0; i < numses; i++) {
        pthread_create(&st[i], 0, test_session, (void*)(long)i);
    }
    for (i = 0; i < numses; i++) pthread_join(st[i], 0);

    sem_destroy(&sm_start);
    sem_destroy(&sm_ready);
    free(st);

    printf("MULTI-SESSION TEST COMPLETE\n");
    return 0;
}
