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
 * @brief UNVMe simple write-read-verify test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <error.h>
#include <time.h>

#include "unvme.h"


int main(int argc, char** argv)
{
    const char* usage =
    "Usage: %s [OPTION]... pciname\n\
             -n       nsid (default 1)\n\
             -q       queue count (default 4)\n\
             -d       queue depth (default 8)\n\
             -s       data size (default 100M)\n\
             pciname  PCI device name (as BB:DD.F format)\n";

    int opt, nsid = 1, qcount = 4, qsize = 8;
    u64 datasize = 100 * 1024 * 1024;
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    while ((opt = getopt(argc, argv, "n:q:d:s:")) != -1) {
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
        case 's':
            datasize = atol(optarg);
            int l = strlen(optarg) - 1;
            if (tolower(optarg[l]) == 'k') datasize *= 1024;
            else if (tolower(optarg[l]) == 'm') datasize *= 1024 * 1024;
            else if (tolower(optarg[l]) == 'g') datasize *= 1024 * 1024 * 1024;
            break;
        default:
            error(1, 0, usage, prog);
        }
    }
    if (optind >= argc) error(1, 0, usage, prog);
    char* pciname = argv[optind];

    printf("SIMPLE WRITE-READ-VERIFY TEST BEGIN\n");
    const unvme_ns_t* ns = unvme_open(pciname, nsid, qcount, qsize);
    if (!ns) error(1, 0, "unvme_open %s failed", argv[0]);
    printf("nsid=%d qc=%d qd=%d ds=%ld cap=%ld mbio=%d\n",
            nsid, qcount, qsize, datasize, ns->blockcount, ns->maxbpio);

    void* buf = unvme_alloc(ns, datasize);
    if (!buf) error(1, 0, "unvme_alloc %ld failed", datasize);

    u64 slba = 0;
    u64 nlb = datasize / ns->blocksize;
    u64* p = buf;
    u64 wsize = datasize / sizeof(u64);
    u64 pat, w;
    int q;
    for (q = 0; q < qcount; q++) {
        pat = time(0);
        printf("Test q=%d buf=%p lba=%#lx nlb=%ld (%08lX)\n", q, p, slba, nlb, pat);
        for (w = 0; w < wsize; w++) p[w] = (pat << 32) + w + q;
        if (unvme_write(ns, q, p, slba, nlb))
            error(1, 0, "unvme_write %ld block(s) failed", nlb);
        memset(p, 0, nlb * ns->blocksize);
        if (unvme_read(ns, q, p, slba, nlb))
            error(1, 0, "unvme_read %ld block(s) failed", nlb);
        for (w = 0; w < wsize; w++) {
            if (p[w] != ((pat << 32) + w + q))
                error(1, 0, "mismatch at lba %#lx word %ld", slba, w);
        }
        slba += nlb;
    }

    unvme_free(ns, buf);
    unvme_close(ns);
    printf("SIMPLE WRITE-READ-VERIFY TEST COMPLETE\n");
    return 0;
}
