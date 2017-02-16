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
 * @brief UNVMe core common functions.
 */

#include <sys/mman.h>
#include <string.h>
#include <signal.h>
#include <sched.h>

#include "unvme_core.h"


/// @cond

#define PDEBUG(fmt, arg...) //fprintf(stderr, fmt "\n", ##arg)

#define LIST_ADD(head, node)                                    \
            if ((head) != NULL) {                               \
                (node)->next = (head);                          \
                (node)->prev = (head)->prev;                    \
                (head)->prev->next = (node);                    \
                (head)->prev = (node);                          \
            } else {                                            \
                (node)->next = (node)->prev = (node);           \
                (head) = (node);                                \
            }
                
#define LIST_DEL(head, node)                                    \
            if ((node->next) != (node)) {                       \
                (node)->next->prev = (node)->prev;              \
                (node)->prev->next = (node)->next;              \
                if ((head) == (node)) (head) = (node)->next;    \
            } else {                                            \
                (head) = NULL;                                  \
            }

/// @endcond

/// Log and print an unrecoverable error message and exit
#define FATAL(fmt, arg...)  do { ERROR(fmt, ##arg); abort(); } while (0)


// Global variables
unvme_device_t      unvme_dev;                              ///< device
static const char*  unvme_logname = "/dev/shm/unvme.log";   ///< log filename


/**
 * Create a namespace object.
 * @param   ses         session
 * @param   nsid        namespace id
 */
static void unvme_ns_init(unvme_session_t* ses, int nsid)
{
    unvme_ns_t* ns = &ses->ns;
    ns->maxqsize = unvme_dev.nvmedev->maxqsize;
    ns->pageshift = unvme_dev.nvmedev->pageshift;
    ns->pagesize = 1 << ns->pageshift;

    vfio_dma_t* dma = vfio_dma_alloc(unvme_dev.vfiodev, ns->pagesize << 1);
    if (!dma) FATAL("vfio_dma_alloc");
    if (nvme_acmd_identify(unvme_dev.nvmedev, nsid, dma->addr,
                           dma->addr + ns->pagesize))
        FATAL("nvme_acmd_identify");

    if (nsid == 0) {
        int i;
        nvme_identify_ctlr_t* idc = (nvme_identify_ctlr_t*)dma->buf;
        ns->vid = idc->vid;
        memcpy(ns->sn, idc->sn, sizeof(ns->sn));
        for (i = sizeof(ns->sn) - 1; i > 0 && ns->sn[i] == ' '; i--) ns->sn[i] = 0;
        memcpy(ns->mn, idc->mn, sizeof(ns->mn));
        for (i = sizeof(ns->mn) - 1; i > 0 && ns->mn[i] == ' '; i--) ns->mn[i] = 0;
        memcpy(ns->fr, idc->fr, sizeof(ns->fr));
        for (i = sizeof(ns->fr) - 1; i > 0 && ns->fr[i] == ' '; i--) ns->fr[i] = 0;
        ns->maxppio = ns->pagesize / sizeof(u64); // limit to 1 PRP list page
        if (idc->mdts) {
            int maxp = 2;
            for (i = 1; i < idc->mdts; i++) maxp *= 2;
            if (ns->maxppio > maxp) ns->maxppio = maxp;
        }

        nvme_feature_num_queues_t nq;
        if (nvme_acmd_get_features(unvme_dev.nvmedev, nsid,
                                   NVME_FEATURE_NUM_QUEUES, 0, 0, (u32*)&nq))
            FATAL("nvme_acmd_get_features");
        ns->maxqcount = (nq.nsq < nq.ncq ? nq.nsq : nq.ncq) + 1;
    } else {
        memcpy(ns, &unvme_dev.ses->ns, sizeof(unvme_ns_t));
        nvme_identify_ns_t* idns = (nvme_identify_ns_t*)dma->buf;
        ns->blockcount = idns->ncap;
        ns->blockshift = idns->lbaf[idns->flbas & 0xF].lbads;
        ns->blocksize = 1 << ns->blockshift;
        if (ns->blocksize > ns->pagesize || ns->blockcount < 8) {
            FATAL("ps=%d bs=%d bc=%ld",
                  ns->pagesize, ns->blocksize, ns->blockcount);
        }
        ns->nbpp = ns->pagesize / ns->blocksize;
        ns->maxbpio = ns->maxppio * ns->nbpp;
        ns->maxiopq = ses->qsize - 1;
    }
    ns->id = nsid;
    ns->ses = ses;
    ns->qcount = ses->qcount;
    ns->qsize = ses->qsize;

    if (vfio_dma_free(dma)) FATAL("vfio_dma_free");
}

/**
 * Get a descriptor entry by moving from the free to the use list.
 * @param   ioq     IO queue
 * @return  the descriptor added to the use list.
 */
static unvme_desc_t* unvme_get_desc(unvme_queue_t* ioq)
{
    unvme_desc_t* desc;

    if (ioq->descfree) {
        desc = ioq->descfree;
        LIST_DEL(ioq->descfree, desc);
    } else {
        desc = zalloc(sizeof(unvme_desc_t) + ioq->ses->masksize);
        desc->ioq = ioq;
    }
    LIST_ADD(ioq->desclist, desc);

    if (desc == desc->next) {
        desc->id = 1;
        ioq->descnext = desc;
    } else {
        desc->id = desc->prev->id + 1;
    }
    ioq->desccount++;

    return desc;
}

/**
 * Put a descriptor entry back by moving it from the use to the free list.
 * @param   desc    descriptor
 */
static void unvme_put_desc(unvme_desc_t* desc)
{
    unvme_queue_t* ioq = desc->ioq;

    if (ioq->descnext == desc) {
        if (desc != desc->next) ioq->descnext = desc->next;
        else ioq->descnext = NULL;
    }

    LIST_DEL(ioq->desclist, desc);
    memset(desc, 0, sizeof(unvme_desc_t) + ioq->ses->masksize);
    desc->ioq = ioq;
    LIST_ADD(ioq->descfree, desc);

    ioq->desccount--;
}

/**
 * Create an I/O queue.
 * @param   ses         session
 * @param   sqi         session queue id
 */
static void unvme_ioq_create(unvme_session_t* ses, int sqi)
{
    unvme_queue_t* ioq = &ses->queues[sqi];
    ioq->ses = ses;

    if (sqi == 0) {
        ses->id = ses->prev->queues[ses->prev->qcount-1].id + 1;
        ses->ns.sid = ses->id;
    }
    ioq->id = ses->id + sqi;
    DEBUG_FN("%x: q=%d qs=%d", unvme_dev.vfiodev->pci, ioq->id, ses->qsize);

    int i;
    for (i = 0; i < 16; i++) unvme_get_desc(ioq);
    ioq->descfree = ioq->desclist;
    ioq->desclist = NULL;
    ioq->desccount = 0;
    ioq->cidmask = zalloc(ses->masksize);

    // assume maxppio fits 1 PRP list page
    ioq->prpsize = ses->ns.pagesize;
    ioq->prplist = vfio_dma_alloc(unvme_dev.vfiodev, ioq->prpsize * ses->qsize);
    if (!ioq->prplist) FATAL("vfio_dma_alloc");
    ioq->sqdma = vfio_dma_alloc(unvme_dev.vfiodev,
                                ses->qsize * sizeof(nvme_sq_entry_t));
    if (!ioq->sqdma) FATAL("vfio_dma_alloc");
    ioq->cqdma = vfio_dma_alloc(unvme_dev.vfiodev,
                                ses->qsize * sizeof(nvme_cq_entry_t));
    if (!ioq->cqdma) FATAL("vfio_dma_alloc");

    ioq->nvq = nvme_create_ioq(unvme_dev.nvmedev, ioq->id, ses->qsize,
                               ioq->sqdma->buf, ioq->sqdma->addr,
                               ioq->cqdma->buf, ioq->cqdma->addr);
    if (!ioq->nvq) FATAL("nvme_create_ioq %d", ioq->id);

    unvme_dev.numioqs++;

    DEBUG_FN("%x: q=%d qc=%d qs=%d db=%#04lx", unvme_dev.vfiodev->pci,
             ioq->nvq->id, unvme_dev.numioqs, ioq->nvq->size,
             (u64)ioq->nvq->sq_doorbell - (u64)unvme_dev.nvmedev->reg);
}

/**
 * Delete an I/O queue.
 * @param   ioq         io queue
 */
static void unvme_ioq_delete(unvme_queue_t* ioq)
{
    DEBUG_FN("%x: q=%d", unvme_dev.vfiodev->pci, ioq->id);
    if (ioq->nvq) (void) nvme_delete_ioq(ioq->nvq);
    if (ioq->prplist) (void) vfio_dma_free(ioq->prplist);
    if (ioq->cqdma) (void) vfio_dma_free(ioq->cqdma);
    if (ioq->sqdma) (void) vfio_dma_free(ioq->sqdma);
    if (ioq->cidmask) free(ioq->cidmask);

    unvme_desc_t* desc;
    while ((desc = ioq->desclist) != NULL) {
        LIST_DEL(ioq->desclist, desc);
        free(desc);
    }
    while ((desc = ioq->descfree) != NULL) {
        LIST_DEL(ioq->descfree, desc);
        free(desc);
    }

    unvme_dev.numioqs--;
}

/**
 * Setup admin queue.
 * @param   ses         session
 */
static void unvme_adminq_create(unvme_session_t* ses)
{
    DEBUG_FN("%x: qs=%d", unvme_dev.vfiodev->pci, ses->qsize);

    unvme_queue_t* adminq = ses->queues;
    adminq->ses = ses;
    adminq->sqdma = vfio_dma_alloc(unvme_dev.vfiodev,
                                   ses->qsize * sizeof(nvme_sq_entry_t));
    if (!adminq->sqdma) FATAL("vfio_dma_alloc");
    adminq->cqdma = vfio_dma_alloc(unvme_dev.vfiodev,
                                   ses->qsize * sizeof(nvme_cq_entry_t));
    if (!adminq->cqdma) FATAL("vfio_dma_alloc");
    adminq->nvq = nvme_setup_adminq(unvme_dev.nvmedev, ses->qsize,
                                    adminq->sqdma->buf, adminq->sqdma->addr,
                                    adminq->cqdma->buf, adminq->cqdma->addr);
    if (!adminq->nvq) FATAL("nvme_setup_adminq");
}

/**
 * Delete admin queue.
 * @param   adminq      admin queue
 */
static void unvme_adminq_delete(unvme_queue_t* adminq)
{
    DEBUG_FN("%x", unvme_dev.vfiodev->pci);
    if (adminq->sqdma) (void) vfio_dma_free(adminq->sqdma);
    if (adminq->cqdma) (void) vfio_dma_free(adminq->cqdma);
}

/**
 * Create a session and its associated queues.
 * @param   nsid        namespace id
 * @param   qcount      queue count
 * @param   qsize       queue size
 * @return  newly created session.
 */
static unvme_session_t* unvme_session_create(int nsid, int qcount, int qsize)
{
    DEBUG_FN("%x: nsid=%d qc=%d qs=%d",
             unvme_dev.vfiodev->pci, nsid, qcount, qsize);
    if ((nsid == 0 && (unvme_dev.ses || qcount != 1)) ||
        (nsid != 0 && !unvme_dev.ses)) FATAL("nsid %d", nsid);

    // allocate a session with its queue array
    unvme_session_t* ses = zalloc(sizeof(unvme_session_t) +
                                  sizeof(unvme_queue_t) * qcount);
    ses->queues = (unvme_queue_t*)(ses + 1);
    ses->qcount = qcount;
    ses->qsize = qsize;
    ses->masksize = ((qsize + 63) / 64) * sizeof(u64);

    if (pthread_spin_init(&ses->iomem.lock, PTHREAD_PROCESS_PRIVATE))
        FATAL("pthread_spin_init");

    LIST_ADD(unvme_dev.ses, ses);
    if (!nsid) {
        unvme_adminq_create(ses);
        unvme_ns_init(ses, nsid);
        DEBUG_FN("%x: adminq", unvme_dev.vfiodev->pci);
    } else {
        unvme_ns_init(ses, nsid);
        int i;
        if (qcount == 0) qcount = ses->ns.maxqcount;
        for (i = 0; i < qcount; i++) unvme_ioq_create(ses, i);
        DEBUG_FN("%x: q=%d-%d bs=%d nb=%lu", unvme_dev.vfiodev->pci,
                 ses->id, ses->queues[qcount-1].id,
                 ses->ns.blocksize, ses->ns.blockcount);
    }

    return ses;
}

/**
 * Delete a session and its associated queues.
 * @param   ses         session
 */
static void unvme_session_delete(unvme_session_t* ses)
{
    if (ses->id > 0) {
        pthread_spin_lock(&ses->iomem.lock);
        if (ses->iomem.size) {
            int i;
            for (i = 0; i < ses->iomem.count; i++) {
                (void) vfio_dma_free(ses->iomem.map[i]);
            }
            ses->iomem.size = ses->iomem.count = 0;
            free(ses->iomem.map);
        }
        pthread_spin_unlock(&ses->iomem.lock);
        pthread_spin_destroy(&ses->iomem.lock);
    }

    if (ses == ses->next) {
        DEBUG_FN("%x: adminq", unvme_dev.vfiodev->pci);
        unvme_adminq_delete(ses->queues);
    } else {
        DEBUG_FN("%x: q=%d-%d", unvme_dev.vfiodev->pci, ses->id,
                                ses->id + ses->qcount -1);
        while (--ses->qcount >= 0) {
            unvme_queue_t* ioq = &ses->queues[ses->qcount];
            if (ioq->ses) unvme_ioq_delete(ioq);
        }
    }
    LIST_DEL(unvme_dev.ses, ses);
    free(ses);
}

/**
 * Initialize and allocate a device array list.
 * @param   pci         PCI device id
 */
static void unvme_init(int pci)
{
    if (log_open(unvme_logname, "w")) exit(1);
    DEBUG_FN();
    //if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0) FATAL("mlockall");
    unvme_dev.vfiodev = vfio_create(pci);
    if (!unvme_dev.vfiodev) FATAL("vfio_create");
    unvme_dev.nvmedev = nvme_create(unvme_dev.vfiodev->fd);
    if (!unvme_dev.nvmedev) FATAL("nvme_create");
    unvme_session_create(0, 1, 8);
    INFO_FN("%x: (%.40s) is ready", pci, unvme_dev.ses->ns.mn);
}

/**
 * Cleanup and exit.
 */
void unvme_cleanup()
{
    INFO_FN();
    while (unvme_dev.ses) unvme_session_delete(unvme_dev.ses->prev);
    if (unvme_dev.nvmedev) nvme_delete(unvme_dev.nvmedev);
    if (unvme_dev.vfiodev) vfio_delete(unvme_dev.vfiodev);
    log_close();
    memset(&unvme_dev, 0, sizeof(unvme_dev));
}

/**
 * Open a new session and create I/O queues.
 * @param   pci         PCI device id
 * @param   nsid        namespace id
 * @param   qcount      number of io queues
 * @param   qsize       size of each queue
 * @return  the new session or NULL if failure.
 */
unvme_session_t* unvme_do_open(int pci, int nsid, int qcount, int qsize)
{
    if (!unvme_dev.vfiodev) unvme_init(pci);
    INFO("%s %x: nsid=%d qc=%d qs=%d", __func__, pci, nsid, qcount, qsize);
    return unvme_session_create(nsid, qcount, qsize);
}

/**
 * Close an I/O session and delete its associated queues.
 * @param   sid         session id
 * @return  0 if ok else -1.
 */
int unvme_do_close(int sid)
{
    INFO("%s %x: sid=%d", __func__, unvme_dev.vfiodev->pci, sid);
    // note first session is admin
    unvme_session_t* ses = unvme_dev.ses->next;
    while (ses != unvme_dev.ses) {
        if (sid == ses->id || sid == 0) {
            unvme_session_delete(ses);
            ses = unvme_dev.ses->next;
        } else {
            ses = ses->next;
        }
    }
    ses = unvme_dev.ses->prev;
    DEBUG_FN("%x: last qid %d", unvme_dev.vfiodev->pci, ses->queues[ses->qcount-1].id);
    if (ses == ses->next) unvme_cleanup();
    return 0;
}

/**
 * Allocate an I/O buffer associated with a session.
 * @param   ses         session
 * @param   size        buffer size
 * @return  the allocated buffer or NULL if failure.
 */
void* unvme_do_alloc(unvme_session_t* ses, u64 size)
{
    void* buf = NULL;

    pthread_spin_lock(&ses->iomem.lock);
    vfio_dma_t* dma = vfio_dma_alloc(unvme_dev.vfiodev, size);
    if (dma) {
        unvme_iomem_t* iomem = &ses->iomem;
        if (iomem->count >= iomem->size) {
            iomem->size += 256;
            iomem->map = realloc(iomem->map, iomem->size * sizeof (vfio_dma_t*));
        }
        iomem->map[iomem->count++] = dma;
        buf = dma->buf;
    }
    pthread_spin_unlock(&ses->iomem.lock);

    return buf;
}

/**
 * Free an I/O buffer associated with a session.
 * @param   ses         session
 * @param   buf         buffer pointer
 * @return  0 if ok else -1.
 */
int unvme_do_free(unvme_session_t* ses, void* buf)
{
    pthread_spin_lock(&ses->iomem.lock);
    vfio_dma_t** map = ses->iomem.map;
    int i;
    for (i = 0; i < ses->iomem.count; i++) {
        if (map[i]->buf == buf) {
            vfio_dma_free(map[i]);
            ses->iomem.count--;
            if (i != ses->iomem.count) map[i] = map[ses->iomem.count];
            pthread_spin_unlock(&ses->iomem.lock);
            return 0;
        }
    }
    pthread_spin_unlock(&ses->iomem.lock);
    ERROR("invalid pointer %p", buf);
    return -1;
}

/**
 * Process an I/O completion.
 * @param   ioq         io queue
 * @param   timeout     timeout in seconds
 * @return  0 if ok else NVMe error code (-1 means timeout).
 */
static int unvme_complete_io(unvme_queue_t* ioq, int timeout)
{
    // wait for completion
    int err, cid;
    u64 endtsc = 0;
    do {
        cid = nvme_check_completion(ioq->nvq, &err);
        if (err) return err;
        if (cid >= 0 || timeout == 0) break;
        if (endtsc == 0) endtsc = rdtsc() + timeout * rdtsc_second();
        else sched_yield();
    } while (rdtsc() < endtsc);
    if (cid < 0) return -1;

    // find the pending cid in the descriptor list to clear it
    unvme_desc_t* desc = ioq->descnext;
    int b = cid >> 6;
    u64 mask = (u64)1 << (cid & 63);
    while ((desc->cidmask[b] & mask) == 0) {
        desc = desc->next;
        if (desc == ioq->descnext) FATAL("pending cid %d not found", cid);
    }
    if (err) desc->error = err;

    desc->cidmask[b] &= ~mask;
    desc->cidcount--;
    ioq->cidmask[b] &= ~mask;
    ioq->cidcount--;
    ioq->cid = cid;

    // check to advance next pending descriptor
    if (ioq->cidcount) {
        while (ioq->descnext->cidcount == 0) ioq->descnext = ioq->descnext->next;
    }
    PDEBUG("# c q%d={%d %d %#lx} @%d={%d %#lx} @%d",
           ioq->id, cid, ioq->cidcount, *ioq->cidmask,
           desc->id, desc->cidcount, *desc->cidmask, ioq->descnext->id);
    return err;
}

/**
 * Submit a single read/write command within the device limit.
 * @param   desc        descriptor
 * @param   buf         data buffer
 * @param   slba        starting lba
 * @param   nlb         number of logical blocks
 * @return  cid if ok else -1.
 */
static int unvme_submit_io(unvme_desc_t* desc, void* buf, u64 slba, u32 nlb)
{
    unvme_queue_t* ioq = desc->ioq;
    unvme_session_t* ses = ioq->ses;
    unvme_ns_t* ns = &ses->ns;
    if (nlb > ns->maxbpio) {
        ERROR("block count %d exceeds limit %d", nlb, ns->maxbpio);
        return -1;
    }

    // find DMA buffer address
    vfio_dma_t* dma = NULL;
    int i;
    pthread_spin_lock(&ses->iomem.lock);
    for (i = 0; i < ses->iomem.count; i++) {
        dma = ses->iomem.map[i];
        if (dma->buf <= buf && buf < (dma->buf + dma->size)) break;
    }
    if (i == ses->iomem.count) {
        ERROR("invalid I/O buffer address");
        pthread_spin_unlock(&ses->iomem.lock);
        return -1;
    }
    pthread_spin_unlock(&ses->iomem.lock);

    int nbpp = ns->nbpp;
    u64 addr = dma->addr + (u64)(buf - dma->buf);
    if ((addr & (ns->blocksize - 1)) != 0) {
        ERROR("unaligned buffer address");
        return -1;
    }
    if ((addr + nlb * ns->blocksize) > (dma->addr + dma->size)) {
        ERROR("buffer overrun");
        return -1;
    }

    // find a free cid
    // if submission queue is full then process a pending entry first
    u16 cid;
    if ((ioq->cidcount + 1) < ses->qsize) {
        cid = ioq->cid;
        while (ioq->cidmask[cid >> 6] & ((u64)1 << (cid & 63))) {
            if (++cid >= ses->qsize) cid = 0;
        }
        ioq->cid = cid;
    } else {
        // if process completion error, clear the current pending descriptor
        unvme_desc_t* desc = ioq->descnext;
        int err = unvme_complete_io(ioq, UNVME_TIMEOUT);
        if (err != 0) {
            if (err == -1) FATAL("ioq %d timeout", ioq->id);
            while (desc->cidcount) {
                if (unvme_complete_io(ioq, UNVME_TIMEOUT) == -1) {
                    FATAL("ioq %d timeout", ioq->id);
                }
            }
        }
        cid = ioq->cid;
    }

    // compose PRPs based on cid
    int numpages = (nlb + nbpp - 1) / nbpp;
    u64 prp1 = addr;
    u64 prp2 = 0;
    if (numpages == 2) {
        prp2 = addr + ns->pagesize;
    } else if (numpages > 2) {
        int prpoff = cid * ioq->prpsize;
        u64* prplist = ioq->prplist->buf + prpoff;
        prp2 = ioq->prplist->addr + prpoff;
        for (i = 1; i < numpages; i++) {
            addr += ns->pagesize;
            *prplist++ = addr;
        }
    }

    if (nvme_cmd_rw(ioq->nvq, desc->opc, cid,
                    ns->id, slba, nlb, prp1, prp2) == 0) {
        int b = cid >> 6;
        u64 mask = (u64)1 << (cid & 63);
        ioq->cidmask[b] |= mask;
        ioq->cidcount++;
        desc->cidmask[b] |= mask;
        desc->cidcount++;
        PDEBUG("# %c %#lx %#x q%d={%d %d %#lx} @%d={%d %#lx}",
               desc->opc == NVME_CMD_READ ? 'r' : 'w', slba, nlb,
               ioq->id, cid, ioq->cidcount, *ioq->cidmask,
               desc->id, desc->cidcount, *desc->cidmask);
        return cid;
    }
    return -1;
}

/**
 * Poll for completion status of a request.
 * @param   desc        descriptor
 * @param   timeout     timeout in seconds
 * @return  0 if ok else error status.
 */
int unvme_do_poll(unvme_desc_t* desc, int timeout)
{
    PDEBUG("# POLL @%d={%d %#lx}", desc->id, desc->cidcount, *desc->cidmask);
    int err = 0;
    while (desc->cidcount) {
        if ((err = unvme_complete_io(desc->ioq, timeout)) != 0) break;
    }
    if (desc->id != 0 && desc->cidcount == 0) unvme_put_desc(desc);
    PDEBUG("# q%d +%d", desc->ioq->id, desc->ioq->desccount);
    return err;
}

/**
 * Submit a read/write command that may require multiple I/O submissions
 * and processing some completions.
 * @param   ioq         io queue
 * @param   opc         op code
 * @param   buf         data buffer
 * @param   slba        starting lba
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
unvme_desc_t* unvme_do_submit(unvme_queue_t* ioq, int opc, void* buf, u64 slba, u32 nlb)
{
    unvme_ns_t* ns = &ioq->ses->ns;
    unvme_desc_t* desc = unvme_get_desc(ioq);
    desc->opc = opc;
    desc->buf = buf;
    desc->slba = slba;
    desc->nlb = nlb;

    PDEBUG("# %s %#lx %#x @%d +%d", opc == NVME_CMD_READ ? "READ" : "WRITE",
           slba, nlb, desc->id, ioq->desccount);
    while (nlb) {
        int n = ns->maxbpio;
        if (n > nlb) n = nlb;
        int cid = unvme_submit_io(desc, buf, slba, n);
        if (cid < 0) {
            if (unvme_do_poll(desc, UNVME_TIMEOUT) != 0) {
                FATAL("ioq %d timeout", ioq->id);
            }
            unvme_put_desc(desc);
            return NULL;
        }

        buf += n * ns->blocksize;
        slba += n;
        nlb -= n;
    }

    return desc;
}
