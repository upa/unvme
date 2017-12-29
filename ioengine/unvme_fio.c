/**
 * @file
 * @brief UNVMe fio plugin engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include </usr/include/err.h>

#include "unvme.h"
#include "config-host.h"
#include "fio.h"
#include "optgroup.h"       // since fio 2.4


#define TDEBUG(fmt, arg...) //fprintf(stderr, "#%s.%d " fmt "\n", __func__, td->thread_number, ##arg)
#define FATAL(fmt, arg...)  do { warnx(fmt, ##arg); abort(); } while (0)


/// Context used for thread initialization
typedef struct {
    pthread_mutex_t     mutex;
    const unvme_ns_t*   ns;
    int                 ncpus;
} unvme_context_t;

/// Thread IO completion queue
typedef struct io_u     *unvme_iocq_t;


// Static variables
static unvme_context_t  unvme = { .mutex = PTHREAD_MUTEX_INITIALIZER };


/*
 * Clean up UNVMe upon exit.
 */
static void do_unvme_cleanup(void)
{
    unvme_close(unvme.ns);
    unvme.ns = NULL;
}

/*
 * Initialize UNVMe once.
 */
static void do_unvme_init(struct thread_data *td)
{
    TDEBUG("device=%s numjobs=%d iodepth=%d", td->o.filename, td->o.numjobs, td->o.iodepth);

    pthread_mutex_lock(&unvme.mutex);

    if (!unvme.ns) {
        char pciname[32];
        int b, d, f, n=1;
        sscanf(td->o.filename, "%x.%x.%x.%x", &b, &d, &f, &n);
        sprintf(pciname, "%x:%x.%x/%x", b, d, f, n);
        unvme.ns = unvme_open(pciname);

        if (!unvme.ns)
            FATAL("unvme_open %s failed", pciname);
        if (td->o.iodepth >= unvme.ns->qsize)
            FATAL("iodepth %d greater than queue size", td->o.iodepth);

        unvme.ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        printf("unvme_open %s q=%dx%d ncpus=%d\n",
               unvme.ns->device, unvme.ns->qcount, unvme.ns->qsize, unvme.ncpus);

        atexit(do_unvme_cleanup);
    }

    if (td->thread_number > unvme.ns->qcount ||
        td->o.iodepth >= unvme.ns->qsize) {
        FATAL("thread %d iodepth %d exceeds UNVMe queue limit %dx%d",
              td->thread_number, td->o.iodepth, unvme.ns->qcount, unvme.ns->qsize-1); 
    }

    pthread_mutex_unlock(&unvme.mutex);
}

/*
 * The ->init() function is called once per thread/process, and should set up
 * any structures that this io engine requires to keep track of io. Not
 * required.
 */
static int fio_unvme_init(struct thread_data *td)
{
    unvme_iocq_t* iocq = calloc(td->o.iodepth, sizeof(void*));
    if (!iocq) return 1;
    td->io_ops_data = iocq;
    return 0;
}

/*
 * This is paired with the ->init() function and is called when a thread is
 * done doing io. Should tear down anything setup by the ->init() function.
 * Not required.
 */
static void fio_unvme_cleanup(struct thread_data *td)
{
    if (td->io_ops_data) free(td->io_ops_data);
    td->io_ops_data = NULL;
}

/*
 * Hook for opening the given file. Unless the engine has special
 * needs, it usually just provides generic_file_open() as the handler.
 */
static int fio_unvme_open(struct thread_data *td, struct fio_file *f)
{
    TDEBUG();
    return 0;
}

/*
 * Hook for closing a file. See fio_unvme_open().
 */
static int fio_unvme_close(struct thread_data *td, struct fio_file *f)
{
    TDEBUG();
    return 0;
}

/*
 * Allocate IO memory.
 */
static int fio_unvme_iomem_alloc(struct thread_data *td, size_t len)
{
    // FIO bug - found cases where this is called before ->get_file_size()
    if (!unvme.ns) do_unvme_init(td);

    if (!td->orig_buffer) td->orig_buffer = unvme_alloc(unvme.ns, len);
    TDEBUG("%p %#lx", td->orig_buffer, len);
    return td->orig_buffer == NULL;
}

/*
 * Free IO memory.
 */
static void fio_unvme_iomem_free(struct thread_data *td)
{
    TDEBUG("%p", td->orig_buffer);
    if (td->orig_buffer) unvme_free(unvme.ns, td->orig_buffer);
}

/*
 * The ->event() hook is called to match an event number with an io_u.
 * After the core has called ->getevents() and it has returned eg 3,
 * the ->event() hook must return the 3 events that have completed for
 * subsequent calls to ->event() with [0-2]. Required.
 */
static struct io_u* fio_unvme_event(struct thread_data *td, int event)
{
    unvme_iocq_t* iocq = td->io_ops_data;
    TDEBUG("GET.%d %p", event, iocq[event]->buf);
    return iocq[event];
}

/*
 * The ->getevents() hook is used to reap completion events from an async
 * io engine. It returns the number of completed events since the last call,
 * which may then be retrieved by calling the ->event() hook with the event
 * numbers. Required.
 */
static int fio_unvme_getevents(struct thread_data *td, unsigned int min,
                               unsigned int max, const struct timespec *t)
{
    int i;
    struct io_u* io_u;
    unvme_iocq_t* iocq = td->io_ops_data;
    int ec = 0;

    for (;;) {
        io_u_qiter(&td->io_u_all, io_u, i) {
            if (io_u->engine_data) {
                int stat = unvme_apoll(io_u->engine_data, 0);
                if (stat == 0) {
                    io_u->engine_data = NULL;
                    TDEBUG("PUT.%d %p (%d %d)", ec, io_u->buf, min, max);
                    iocq[ec++] = io_u;
                    if (ec == max) return ec;
                } else if (stat == -1) {
                    if (ec >= min) return ec;
                } else {
                    FATAL("\nunvme_apoll return %#x", stat);
                }
            }
        }
    }

    return 0;
}


#ifndef _UNVME_MMCC_H

/*
 * The ->queue() hook is responsible for initiating io on the io_u
 * being passed in. If the io engine is a synchronous one, io may complete
 * before ->queue() returns. Required.
 *
 * The io engine must transfer in the direction noted by io_u->ddir
 * to the buffer pointed to by io_u->xfer_buf for as many bytes as
 * io_u->xfer_buflen. Residual data count may be set in io_u->resid
 * for a short read/write.
 */
static int fio_unvme_queue(struct thread_data *td, struct io_u *io_u)
{
    int q = td->thread_number - 1;
    void* buf = io_u->buf;
    u64 slba = io_u->offset >> unvme.ns->blockshift;
    int nlb = io_u->xfer_buflen >> unvme.ns->blockshift;

    switch (io_u->ddir) {
    case DDIR_READ:
        TDEBUG("READ q%d %p %#lx %d", q, buf, slba, nlb);
        if ((io_u->engine_data = unvme_aread(unvme.ns, q, buf, slba, nlb)))
            return FIO_Q_QUEUED;
        FATAL("\nunvme_aread q=%d slba=%#lx nlb=%d", q, slba, nlb);
        break;

    case DDIR_WRITE:
        TDEBUG("WRITE q%d %p %#lx %d", q, buf, slba, nlb);
        if ((io_u->engine_data = unvme_awrite(unvme.ns, q, buf, slba, nlb)))
            return FIO_Q_QUEUED;
        FATAL("\nunvme_awrite q=%d slba=%#lx nlb=%d", q, slba, nlb);
        break;

    default:
        break;
    }

    return FIO_Q_COMPLETED;
}

/*
 * The ->get_file_size() is called once for every job (i.e. numjobs)
 * before all other functions.  This is called after ->setup() but
 * is simpler to initialize here since we only care about the device name
 * (given as file_name) and just have to specify the device size.
 */
static int fio_unvme_get_file_size(struct thread_data *td, struct fio_file *f)
{
    TDEBUG("file=%s", f->file_name);
    if (!fio_file_size_known(f)) {
        do_unvme_init(td);
        f->filetype = FIO_TYPE_CHAR;
        f->real_file_size = unvme.ns->blockcount * unvme.ns->blocksize;
        fio_file_set_size_known(f);
    }
    return 0;
}


// Note that the structure is exported, so that fio can get it via
// dlsym(..., "ioengine");
struct ioengine_ops ioengine = {
    .name               = "unvme",
    .version            = FIO_IOOPS_VERSION,
    .get_file_size      = fio_unvme_get_file_size,
    .init               = fio_unvme_init,
    .cleanup            = fio_unvme_cleanup,
    .open_file          = fio_unvme_open,
    .close_file         = fio_unvme_close,
    .iomem_alloc        = fio_unvme_iomem_alloc,
    .iomem_free         = fio_unvme_iomem_free,
    .queue              = fio_unvme_queue,
    .getevents          = fio_unvme_getevents,
    .event              = fio_unvme_event,
    .flags              = FIO_NOEXTEND | FIO_RAWIO,
};

#endif // _UNVME_MMCC_H

