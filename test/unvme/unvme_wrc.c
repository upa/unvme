/**
 * @file
 * @brief Device read/write utility.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <err.h>

#include "unvme.h"

#define PDEBUG(fmt, arg...)     //printf(fmt "\n", ##arg)

/// IO submission structure
typedef struct {
    unvme_iod_t             iod;                ///< IO descriptor
    void*                   buf;                ///< buffer
    u64                     lba;                ///< address
    int                     nlb;                ///< number of blocks
    int                     q;                  ///< queue
} sio_t;

// Global static variables
static const unvme_ns_t*    ns;             ///< namespace handle
static char*                pciname;        ///< device name
static u32                  rw = 0;         ///< read-write flag
static u64                  startlba = 0;   ///< starting LBA
static u64                  lbacount = 0;   ///< LBA count
static u64                  pattern = 0;    ///< 64-bit data pattern
static u64                  patinc = 0;     ///< pattern increment per LBA
static u32                  maxioc = 0;     ///< max IO submission count
static u32                  qcount = 0;     ///< IO queue count
static u32                  qsize = 0;      ///< IO queue size
static u32                  nbpio = 0;      ///< number of blocks per IO
static time_t               dumpitv = 0;    ///< second interval to display data
static int                  mismatch = 0;   ///< data miscompare flag
static int                  wib = 0;        ///< number of 64-bit words in a block
static sio_t*               sios;           ///< array IO descriptors
static void**               iobufs;         ///< array of IO buffers
static u64*                 fixedbuf;       ///< fixed data block buffer


/*
 * Parse arguments.
 */
static void parse_args(int argc, char** argv)
{
    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt, err = 0;

    while ((opt = getopt(argc, argv, "w:r:i:a:n:q:d:m:p:R")) != -1) {
        switch (opt) {
        case 'w':
        case 'r':
        case 'R':
            rw = opt;
            if (opt != 'R') pattern = strtoull(optarg, 0, 0);
            break;
        case 'i':
            patinc = strtoull(optarg, 0, 0);
            break;
        case 'a':
            startlba = strtoull(optarg, 0, 0);
            break;
        case 'n':
            lbacount = strtoull(optarg, 0, 0);
            break;
        case 'q':
            qcount = strtoul(optarg, 0, 0);
            break;
        case 'd':
            qsize = strtoul(optarg, 0, 0);
            break;
        case 'm':
            nbpio = strtoul(optarg, 0, 0);
            break;
        case 'p':
            dumpitv = strtoul(optarg, 0, 0);
            break;
        default:
            err = 1;
        }
    }

    if (err == 0 && rw != 0 && ((optind + 1) == argc)) {
        pciname = argv[optind];
        return;
    }

    fprintf(stderr,  "Usage: %s [OPTION]... PCINAME\n\
         -w PATTERN   write the specified (64-bit) data pattern\n\
         -r PATTERN   read and compare against the specified data pattern\n\
         -R           read without verifying data\n\
         -i PATINC    increment data pattern at each LBA (default 0)\n\
         -a LBA       starting at LBA (default 0)\n\
         -n COUNT     number of blocks to read/write (default to end)\n\
         -q QCOUNT    use number of queues for async IO (default max support)\n\
         -d QSIZE     use queue size for async IO (default %d)\n\
         -m NBPIO     use number of blocks per IO (default max support)\n\
         -p INTERVAL  print progress with LBA data every INTERVAL seconds\n\
         PCINAME      PCI device name (as 01:00.0[/1] format)\n\n\
         either -w or -r or -R must be specified", prog, UNVME_QSIZE);

    exit(1);
}


/*
 * Dump buffer content in hex.
 */
static void dump_block(void* buf, u64 lba)
{
    printf("===== LBA 0x%lx =====\n", lba);
    u64* p = buf;
    u64 w0 = ~*p, w1 = 0, w2 = 0, w3 = 0;
    int i, len = ns->blocksize, skip = 0;
    for (i = 0; i < len; i += 32) {
        if (p[0] != w0 || p[1] != w1 || p[2] != w2 || p[3] != w3) {
            printf("%04x: %016lx %016lx %016lx %016lx\n",
                   i, p[0], p[1], p[2], p[3]);
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

/*
 * Submit next IO to a queue.
 */
void submit_io(sio_t* sio)
{
    static time_t tdump = 0x7fffffffffffffffL;

    if (rw == 'w') {
        int b, i;
        int nlb = sio->nlb;
        if (patinc) {
            u64* pbuf = sio->buf;
            int wib = ns->blocksize / sizeof(u64);
            for (b = 0; b < nlb; b++) {
                u64 p = pattern + ((b + sio->lba - startlba) * patinc);
                for (i = 0; i < wib; i++) *pbuf++ = p;
            }
        }

        PDEBUG("@W q%d %p %#lx %u", sio->q, sio->buf, sio->lba, sio->nlb);
        sio->iod = unvme_awrite(ns, sio->q, sio->buf, sio->lba, sio->nlb);
        if (!sio->iod) errx(1, "unvme_awrite q=%d lba=%#lx nlb=%#x failed", sio->q, sio->lba, sio->nlb);

        // check to dump content on specified interval
        if (dumpitv) {
            time_t t = time(0);
            if ((tdump - t) >= dumpitv) {
                tdump = t;
                void* buf = sio->buf;
                for (b = 0; b < nlb; b++) {
                    dump_block(buf, sio->lba + b);
                    buf += ns->blocksize;
                }
            }
        }
    } else {
        PDEBUG("@R q%d %p %#lx %u", sio->q,  sio->buf, sio->lba, sio->nlb);
        sio->iod = unvme_aread(ns, sio->q, sio->buf, sio->lba, sio->nlb);
        if (!sio->iod) errx(1, "unvme_aread q=%d lba=%#lx nlb=%#x failed", sio->q, sio->lba, sio->nlb);
    }
}

/*
 * Check IO and compare data if read.
 * Return number of blocks completed (0 if none).
 */
int check_io(sio_t* sio)
{
    static time_t tdump = 0x7fffffffffffffffL;

    int stat = unvme_apoll(sio->iod, 0);
    if (stat == -1) return 0;
    if (stat != 0) errx(1, "unvme_apoll status %#x lba=%#lx nlb=%#x", stat, sio->lba, sio->nlb);

    int nlb = sio->nlb;

    switch (rw) {
    case 'w':
        PDEBUG("@WC q%d %p %#lx %d", sio->q, sio->buf, sio->lba, nlb);
        break;

    case 'r':
        PDEBUG("@RC q%d %p %#lx %d", sio->q, sio->buf, sio->lba, nlb);
        if (mismatch) break;

        // compare read result unless there's already a data mismatch
        void* buf;
        u64 lba;
        int b;

        // check to dump content on specified interval
        if (dumpitv) {
            buf = sio->buf;
            lba = sio->lba;
            time_t t = time(0);
            if ((tdump - t) >= dumpitv) {
                tdump = t;
                int b;
                for (b = 0; b < nlb; b++) {
                    dump_block(buf, lba + b);
                    buf += ns->blocksize;
                }
            }
        }

        // compare read results against data pattern
        buf = sio->buf;
        lba = sio->lba;
        if (patinc) {
            for (b = 0; b < nlb; b++) {
                u64 pat = pattern + ((lba - startlba) * patinc);
                u64* pbuf = buf;
                int i;
                for (i = 0; i < wib; i++) {
                    if (*pbuf != pat) {
                        dump_block(buf, lba);
                        warnx("ERROR: data mismatch at LBA %#lx "
                                "offset %#lx exp %#016lx obs %#016lx",
                                lba, i * sizeof(u64), pat, *pbuf);
                        mismatch++;
                        break;
                    }
                    pbuf++;
                }
                if (mismatch) break;
                buf += ns->blocksize;
                lba++;
            }
        } else {
            for (b = 0; b < nlb; b++) {
                if (memcmp(buf, fixedbuf, ns->blocksize)) {
                    dump_block(buf, lba);
                    warnx("ERROR: data mismatch at LBA %#lx exp %#016lx",
                            lba, pattern);
                    mismatch++;
                    break;
                }
                buf += ns->blocksize;
                lba++;
            }
        }

    case 'R':
        PDEBUG("@RC q%d %p %#lx %d", sio->q, sio->buf, sio->lba, nlb);
        break;

    default:
        errx(1, "unknown command %c", rw);
        break;
    }

    sio->iod = NULL;
    return nlb;
}

/*
 * Main.
 */
int main(int argc, char** argv)
{
    parse_args(argc, argv);

    // open device and allocate buffers
    time_t tstart = time(0);
    ns = unvme_openq(pciname, qcount, qsize);
    if (!ns) exit(1);
    if ((startlba + lbacount) > ns->blockcount) {
        unvme_close(ns);
        errx(1, "max block count is %#lx", ns->blockcount);
    }
    if (lbacount == 0) lbacount = ns->blockcount - startlba;
    if (nbpio == 0) nbpio = ns->maxbpio;
    if (qcount == 0) qcount = ns->qcount;
    if (qsize == 0) qsize = ns->qsize;

    printf("%s qc=%d/%d qs=%d/%d bc=%#lx bs=%d nbpio=%d/%d\n",
            ns->device, qcount, ns->maxqcount, qsize, ns->maxqsize,
            ns->blockcount, ns->blocksize, nbpio, ns->maxbpio);

    if (nbpio > ns->maxbpio || (nbpio % ns->nbpp)) {
        unvme_close(ns);
        errx(1, "invalid nbpio %d", nbpio);
    }

    // allocate and setup submission IO queues and buffers
    maxioc = qcount * ns->maxiopq;
    u32 numioc = lbacount / nbpio;
    if ((lbacount % nbpio) != 0) numioc++;
    if (maxioc > numioc) maxioc = numioc;
    sios = calloc(maxioc, sizeof(sio_t));
    iobufs = calloc(maxioc, sizeof(void*));
    u64 iobufsize = nbpio * ns->blocksize;
    int i;
    for (i = 0; i < maxioc; i++) {
        iobufs[i] = unvme_alloc(ns, iobufsize);
        if (!iobufs[i]) errx(1, "unvme_alloc %#lx failed", iobufsize);
        sios[i].buf = iobufs[i];
        sios[i].q = i % ns->qcount;
    }

    // setup a fixed pattern buffer
    wib = ns->blocksize / sizeof(u64);
    if (patinc == 0) {
        fixedbuf = malloc(ns->blocksize);
        for (i = 0; i < wib; i++) fixedbuf[i] = pattern;
    }

    // setup for write and read
    switch (rw) {
    case 'w':
        printf("WRITE lba=%#lx-%#lx pat=%#lx inc=%#lx\n",
               startlba, startlba + lbacount - 1, pattern, patinc);

        // if fixed pattern then fill all buffers with the pattern
        if (patinc == 0) {
            for (i = 0; i < maxioc; i++) {
                void* buf = iobufs[i];
                int b;
                for (b = 0; b < nbpio; b++) {
                    memcpy(buf, fixedbuf, ns->blocksize);
                    buf += ns->blocksize;
                }
            }
        }
        break;

    case 'r':
        printf("READ lba=%#lx-%#lx COMPARE pat=%#lx inc=%#lx\n",
               startlba, startlba + lbacount - 1, pattern, patinc);
        break;

    case 'R':
        printf("READ lba=%#lx-%#lx\n", startlba, startlba + lbacount - 1);
        break;

    default:
        errx(1, "unknown command %c", rw);
        break;
    }

    // submit async IOs until all are completed
    long submit = lbacount;
    long complete = lbacount;
    u64 nextlba = startlba;
    int k = 0;

    for (;;) {
        sio_t* sio = &sios[k];
        if (sio->iod == NULL) {
            if (submit > 0) {
                int nlb = nbpio;
                if (nlb > submit) nlb = submit;
                sio->lba = nextlba;
                sio->nlb = nlb;
                submit_io(sio);
                nextlba += nlb;
                submit -= nlb;
            }
        } else {
            int nlb = check_io(sio);
            if (nlb) {
                complete -= nlb;
                if (complete == 0) break;
                if (submit > 0) continue;
            }
        }

        if (++k >= maxioc) k = 0;
    }

    for (i = 0; i < maxioc; i++) unvme_free(ns, iobufs[i]);
    if (patinc) free(fixedbuf);
    free(iobufs);
    free(sios);
    unvme_close(ns);

    if (!mismatch) printf("Completion time: %ld seconds\n", time(0) - tstart);

    return mismatch;
}

