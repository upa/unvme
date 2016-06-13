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
 * @brief UNVMe API test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <error.h>
#include <time.h>

#include "libunvme.h"


/**
 * Main.
 */
int main(int argc, char** argv)
{
    const char* usage =
    "Usage: %s [OPTION]... pciname\n\
             -n       nsid (default 1)\n\
             -q       queue count (default 2)\n\
             -d       queue depth (default 500)\n\
             -r       ratio (default 4)\n\
             -v       verbose\n\
             pciname  PCI device name (as BB:DD.F format)\n";

    int opt, nsid=1, qcount=2, qsize=500, ratio=4, verbose=0;
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    while ((opt = getopt(argc, argv, "n:q:d:r:v")) != -1) {
        switch (opt) {
        case 'n':
            nsid = atoi(optarg);
            if (nsid <= 0) error(1, 0, "n must be > 0");
            break;
        case 'q':
            qcount = atoi(optarg);
            if (qsize <= 0) error(1, 0, "q must be > 0");
            break;
        case 'd':
            qsize = atoi(optarg);
            if (qsize <= 1) error(1, 0, "d must be > 1");
            break;
        case 'r':
            ratio = atoi(optarg);
            if (ratio <= 0) error(1, 0, "r must be > 0");
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            error(1, 0, usage, prog);
        }
    }
    if (optind >= argc) error(1, 0, usage, prog);
    char* pciname = argv[optind];

    printf("API TEST BEGIN\n");
    const unvme_ns_t* ns = unvme_open(pciname, nsid, qcount, qsize);
    if (!ns) error(1, 0, "unvme_open %s failed", argv[0]);

    // set large number of I/O and size
    int maxnlb = ratio * ns->maxbpio;
    int iocount = ratio * qsize;

    printf("open ns=%d qc=%d qd=%d ratio=%d maxnlb=%d/%d cap=%lu\n",
            nsid, qcount, qsize, ratio, maxnlb, ns->maxbpio, ns->blockcount);

    int q, i, nlb;
    u64 slba, size, w, *p;
    unvme_iod_t* iod = malloc(iocount * sizeof(unvme_iod_t));
    void** buf = malloc(iocount * sizeof(void*));

    for (q = 0; q < qcount; q++) {
        printf("\n> Test q=%d ioc=%d\n", q, iocount);
        int t = time(0);

        printf("Test alloc\n");
        srand(t);
        for (i = 0; i < iocount; i++) {
            nlb = rand() % maxnlb + 1;
            size = nlb * ns->blocksize;
            if (verbose) printf("  alloc.%-2d  %#8x %ld\n", i, nlb, size);
            if (!(buf[i] = unvme_alloc(ns, size)))
                error(1, 0, "alloc.%d failed", i);
        }

        printf("Test awrite\n");
        srand(t);
        slba = 0;
        for (i = 0; i < iocount; i++) {
            int nlb = rand() % maxnlb + 1;
            size = nlb * ns->blocksize / sizeof(u64);
            p = buf[i];
            for (w = 0; w < size; w++) p[w] = (w << 32) + i;
            if (verbose) printf("  awrite.%-2d %#8x %p %#lx\n", i, nlb, p, slba);
            if (!(iod[i] = unvme_awrite(ns, q, p, slba, nlb)))
                error(1, 0, "awrite.%d failed", i);
            slba += nlb;
        }

        printf("Test apoll.awrite\n");
        for (i = iocount-1; i >= 0; i--) {
            if (verbose) printf("  apoll.awrite.%-2d\n", i);
            if (unvme_apoll(iod[i], UNVME_TIMEOUT))
                error(1, 0, "apoll_awrite.%d failed", i);
        }

        printf("Test aread\n");
        srand(t);
        slba = 0;
        for (i = 0; i < iocount; i++) {
            nlb = rand() % maxnlb + 1;
            size = nlb * ns->blocksize;
            p = buf[i];
            bzero(p, size);
            if (verbose) printf("  aread.%-2d  %#8x %p %#lx\n", i, nlb, p, slba);
            if (!(iod[i] = unvme_aread(ns, q, p, slba, nlb)))
                error(1, 0, "aread.%d failed", i);
            slba += nlb;
        }

        printf("Test apoll.aread\n");
        for (i = iocount-1; i >= 0; i--) {
            if (verbose) printf("  apoll.aread.%-2d\n", i);
            if (unvme_apoll(iod[i], UNVME_TIMEOUT))
                error(1, 0, "apoll_aread.%d failed", i);
        }

        printf("Test verify\n");
        srand(t);
        slba = 0;
        for (i = 0; i < iocount; i++) {
            nlb = rand() % maxnlb + 1;
            size = nlb * ns->blocksize / sizeof(u64);
            p = buf[i];
            if (verbose) printf("  verify.%-2d %#8x %p %#lx\n", i, nlb, p, slba);
            for (w = 0; w < size; w++) {
                if (p[w] != ((w << 32) + i))
                    error(1, 0, "mismatch lba=%#lx word=%#lx", slba, w);
            }
            slba += nlb;
        }

        printf("Test free\n");
        for (i = 0; i < iocount; i++) {
            if (verbose) printf("  free.%-2d\n", i);
            if (unvme_free(ns, buf[i]))
                error(1, 0, "free.%d failed", i);
        }
    }

    free(buf);
    free(iod);
    unvme_close(ns);
    printf("\nAPI TEST COMPLETE\n");
    return 0;
}
