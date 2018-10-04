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
 * @brief UNVMe write-read a specific address.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <err.h>

#include "unvme.h"


int main(int argc, char** argv)
{
    const char* usage = "Usage: %s -w|-r LBA [OPTIONS]... PCINAME\n\
           -w|-r LBA         write|read at LBA\n\
           -n    BLOCKCOUNT  number of blocks (default 8)\n\
           -p    PATTERN     64-bit data pattern to write (default random)\n\
           PCINAME           PCI device name (as 02:00.0[/1] format)\n\n\
           (either -w or -r must be specified)\n";

    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt;
    int rw = 0;
    u64 lba = 0L;
    u32 nlb = 8;
    u64 pat = 0L;
    int rnd = 1;

    while ((opt = getopt(argc, argv, "w:r:n:p:")) != -1) {
        switch (opt) {
        case 'w':
        case 'r':
            rw = opt;
            lba = strtoull(optarg, 0, 0);
            break;
        case 'n':
            nlb = strtoul(optarg, 0, 0);
            break;
        case 'p':
            pat = strtoull(optarg, 0, 0);
            rnd = 0;
            break;
        default:
            warnx(usage, prog);
            exit(1);
        }
    }

    if ((rw == 0) || (optind + 1) != argc) {
        warnx(usage, prog);
        exit(1);
    }
    char* pciname = argv[optind];

    const unvme_ns_t* ns = unvme_open(pciname);
    if (!ns) exit(1);
    printf("%s qc=%d/%d qs=%d/%d bc=%#lx bs=%d\n",
            ns->device, ns->qcount, ns->maxqcount, ns->qsize, ns->maxqsize,
            ns->blockcount, ns->blocksize);

    int stat;
    u64 i, len = nlb * ns->blocksize;
    void* buf = unvme_alloc(ns, len);
    if (!buf) errx(1, "unvme_alloc %lu failed", len);

    if (rw == 'w') {
        if (rnd) {
            srandom(time(NULL));
            u32* p = buf;
            for (i = 0; i < len; i += sizeof(u32)) *p++ = random();
            printf("Write random pattern at lba %#lx nlb %u\n", lba, nlb);
        } else {
            u64* p = buf;
            printf("Write pattern %#016lx at lba %#lx nlb %u\n", pat, lba, nlb);
            for (i = 0; i < len; i += sizeof(u64)) *p++ = pat;
        }
        int stat = unvme_write(ns, 0, buf, lba, nlb);
        if (stat) errx(1, "unvme_write failed: lba=%#lx nlb=%#x stat=%#x", lba, nlb, stat);
    } else {
        printf("Read lba %#lx nlb %u\n", lba, nlb);
        stat = unvme_read(ns, 0, buf, lba, nlb);
        if (stat) errx(1, "unvme_read failed: lba=%#lx nlb=%#x stat=%#x", lba, nlb, stat);

        int skip = 0;
        u64* p = buf;
        u64 w0 = ~*p, w1 = 0, w2 = 0, w3 = 0;
        for (i = 0; i < len; i += 32) {
            if (p[0] != w0 || p[1] != w1 || p[2] != w2 || p[3] != w3) {
                printf("%08lx: %016lx %016lx %016lx %016lx\n", i, p[0], p[1], p[2], p[3]);
                skip = 0;
            } else if (!skip) {
                printf("*\n");
                skip = 1;
            }
            w0 = p[0];
            w1 = p[1];
            w2 = p[2];
            w3 = p[3];
            p += 4;
        }
    }

    unvme_free(ns, buf);
    unvme_close(ns);
    return 0;
}

