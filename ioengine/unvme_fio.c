/**
 * @file
 * @brief UNVMe fio plugin engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <error.h>
#include <assert.h>
#include <pthread.h>

#include "unvme.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"       // since fio 2.4

#define DEBUG(fmt, arg...)  //fprintf(stderr, "#" fmt "\n", ##arg)
#define TDEBUG(fmt, arg...) DEBUG("%s.%d " fmt, __func__, td->thread_number, ##arg)

typedef struct {
    void*               pad;
    unsigned int        nsid;
    unsigned int        maxjobs;
} unvme_options_t;

typedef struct {
    struct io_u**       iocq;
    int                 head;
    int                 tail;
} unvme_data_t;

typedef struct {
    pthread_mutex_t     mutex;
    const unvme_ns_t*   ns;
    int                 active;
} unvme_context_t;


// Static variables
static unvme_context_t  unvme = { .mutex = PTHREAD_MUTEX_INITIALIZER };


/*
 * Initialize UNVMe.
 */
static int do_unvme_init(char* pciname, struct thread_data *td)
{
    pthread_mutex_lock(&unvme.mutex);
    unvme.active++;
    if (!unvme.ns) {
        unvme_options_t* opt = td->eo;
        int nsid = opt->nsid ? opt->nsid : 1;
        int qc = opt->maxjobs ? opt->maxjobs : td->o.numjobs;
        int qd = td->o.iodepth;

        if (pciname[2] == '.') pciname[2] = ':';
        unvme.ns = unvme_open(pciname, nsid, qc, qd + 1);
        if (unvme.ns) {
            DEBUG("%s unvme_open %s nsid=%d q=%dx%d",
                  __func__, pciname, nsid, qc, qd);
        } else {
            error(0, 0, "unvme_open %s failed", pciname);
            pthread_mutex_unlock(&unvme.mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&unvme.mutex);
    return 0;
}

/*
 * The ->event() hook is called to match an event number with an io_u.
 * After the core has called ->getevents() and it has returned eg 3,
 * the ->event() hook must return the 3 events that have completed for
 * subsequent calls to ->event() with [0-2]. Required.
 */
static struct io_u* fio_unvme_event(struct thread_data *td, int event)
{
    unvme_data_t* udata = td->io_ops_data;
    struct io_u* io_u = NULL;

    if (udata->head != udata->tail) {
        io_u = udata->iocq[udata->head];
        TDEBUG("GET.%d %p", udata->head, io_u->buf);
        if (++udata->head > td->o.iodepth) udata->head = 0;
    }
    return io_u;
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
    unvme_data_t* udata = td->io_ops_data;
    int events = 0;
    struct timespec t0, t1;
    uint64_t timeout = 0;

    if (t) {
        timeout = t->tv_sec * 1000000000L + t->tv_nsec;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    }

    for (;;) {
        struct io_u* io_u;
        int i;

        io_u_qiter(&td->io_u_all, io_u, i)
        {
            if (io_u->engine_data) {
                if (!unvme_apoll(io_u->engine_data, 0)) {
                    io_u->engine_data = NULL;
                    udata->iocq[udata->tail] = io_u;
                    TDEBUG("PUT.%d %p", udata->tail, io_u->buf);
                    if (++udata->tail > td->o.iodepth) udata->tail = 0;
                    if (++events >= min) return events;
                } else if (t) {
                    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
                    uint64_t elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
                                      + t1.tv_nsec - t0.tv_nsec;
                    if (elapse > timeout) return events;
                }
            }
        }
    }

    return 0;
}

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
    /*
     * Double sanity check to catch errant write on a readonly setup
     */
    fio_ro_check(td, io_u);

    int err = 0;
    void* buf = io_u->buf;
    u64 slba = io_u->offset >> unvme.ns->blockshift;
    int nlb = io_u->xfer_buflen >> unvme.ns->blockshift;
    int q = td->thread_number - 1;

    switch (io_u->ddir) {
    case DDIR_READ:
        TDEBUG("READ q%d %p %#lx %d", q, buf, slba, nlb);
        if (!(io_u->engine_data = unvme_aread(unvme.ns, q, buf, slba, nlb)))
            err = 1;
        break;

    case DDIR_WRITE:
        TDEBUG("WRITE q%d %p %#lx %d", q, buf, slba, nlb);
        if (!(io_u->engine_data = unvme_awrite(unvme.ns, q, buf, slba, nlb)))
            err = 1;
        break;

    default:
        break;
    }

    /*
     * Could return FIO_Q_QUEUED for a queued request,
     * FIO_Q_COMPLETED for a completed request, and FIO_Q_BUSY
     * if we could queue no more at this point (you'd have to
     * define ->commit() to handle that.
     */
    return err ? FIO_Q_COMPLETED : FIO_Q_QUEUED;
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
 * The ->init() function is called once per thread/process, and should set up
 * any structures that this io engine requires to keep track of io. Not
 * required.
 */
static int fio_unvme_init(struct thread_data *td)
{
    unvme_data_t* udata = calloc(1, sizeof(unvme_data_t));
    if (!udata) return 1;

    udata->iocq = calloc(td->o.iodepth + 1, sizeof(void*));
    if (!udata->iocq) {
        free (udata);
        return 1;
    }

    td->io_ops_data = udata;
    return 0;
}

/*
 * This is paired with the ->init() function and is called when a thread is
 * done doing io. Should tear down anything setup by the ->init() function.
 * Not required.
 */
static void fio_unvme_cleanup(struct thread_data *td)
{
    unvme_data_t* udata = td->io_ops_data;
    if (udata) {
        if (udata->iocq) free(udata->iocq);
        free(udata);
    }

    pthread_mutex_lock(&unvme.mutex);
    TDEBUG("active=%d", unvme.active);
    if (--unvme.active == 0 && unvme.ns) {
        DEBUG("%s unvme_close", __func__);
        unvme_close(unvme.ns);
        unvme.ns = NULL;
    }
    pthread_mutex_unlock(&unvme.mutex);
}

static int fio_unvme_iomem_alloc(struct thread_data *td, size_t len)
{
    td->orig_buffer = unvme_alloc(unvme.ns, len);
    TDEBUG("%p %ld", td->orig_buffer, len);
    return td->orig_buffer == NULL;
}

static void fio_unvme_iomem_free(struct thread_data *td)
{
    TDEBUG("%p", td->orig_buffer);
    unvme_free(unvme.ns, td->orig_buffer);
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
        if (do_unvme_init(f->file_name, td)) {
            error(0, 0, "%s do_unvme_init", __func__);
            return 1;
        }
        f->filetype = FIO_TYPE_CHAR;
        f->real_file_size = unvme.ns->blockcount * unvme.ns->blocksize;
        fio_file_set_size_known(f);
    }
    return 0;
}


// UNVMe options.
static struct fio_option fio_unvme_options[] = {
    {
        .name       = "nsid",
        .lname      = "NVMe nsid",
        .type       = FIO_OPT_INT,
        .off1       = offsetof(unvme_options_t, nsid),
        .minval     = 1,
        .maxval     = 0xffff,
        .help       = "NVMe namespace id",
        .category   = FIO_OPT_C_ENGINE,
    },
    {
        .name       = "maxjobs",
        .lname      = "Max number of jobs",
        .type       = FIO_OPT_INT,
        .off1       = offsetof(unvme_options_t, maxjobs),
        .minval     = 1,
        .maxval     = 0xffff,
        .help       = "Max number of jobs mapped to number of NVMe queues",
        .category   = FIO_OPT_C_ENGINE,
    },
    {
        .name   = NULL,
    },
};


// Note that the structure is exported, so that fio can get it via
// dlsym(..., "ioengine");
struct ioengine_ops ioengine = {
    .name               = "unvme_fio",
    .version            = FIO_IOOPS_VERSION,
    .queue              = fio_unvme_queue,
    .getevents          = fio_unvme_getevents,
    .event              = fio_unvme_event,
    .init               = fio_unvme_init,
    .cleanup            = fio_unvme_cleanup,
    .open_file          = fio_unvme_open,
    .close_file         = fio_unvme_close,
    .get_file_size      = fio_unvme_get_file_size,
    .iomem_alloc        = fio_unvme_iomem_alloc,
    .iomem_free         = fio_unvme_iomem_free,
    .flags              = FIO_NOEXTEND | FIO_RAWIO,
    .options            = fio_unvme_options,
    .option_struct_size = sizeof(unvme_options_t),
};

