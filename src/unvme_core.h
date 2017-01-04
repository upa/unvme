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
 * @brief UNVMe header file.
 */

#ifndef _UNVME_CORE_H
#define _UNVME_CORE_H

#include <sys/types.h>

#include "rdtsc.h"
#include "unvme_log.h"
#include "unvme_vfio.h"
#include "unvme_nvme.h"
#include "unvme.h"


struct _unvme_session;
struct _unvme_queue;

/// IO memory allocation tracker per session
typedef struct _unvme_iomem {
    pthread_spinlock_t      lock;       ///< lock to access memory
    vfio_dma_t**            map;        ///< dynamic array of allocated memory
    int                     size;       ///< array size
    int                     count;      ///< array count
} unvme_iomem_t;

/// IO descriptor
typedef struct _unvme_desc {
    u32                     id;         ///< descriptor id
    u32                     nlb;        ///< number of blocks
    u64                     slba;       ///< starting lba
    void*                   buf;        ///< buffer
    int                     opc;        ///< op code
    int                     error;      ///< error status
    struct _unvme_desc*     prev;       ///< previous descriptor node
    struct _unvme_desc*     next;       ///< next descriptor node
    struct _unvme_queue*    ioq;        ///< queue owner
    int                     cidcount;   ///< number of pending cids
    u64                     cidmask[];  ///< cid pending bit mask
} unvme_desc_t;

/// queue context
typedef struct _unvme_queue {
    struct _unvme_session*  ses;        ///< session reference
    nvme_queue_t*           nvq;        ///< NVMe associated queue
    vfio_dma_t*             sqdma;      ///< submission queue allocation
    vfio_dma_t*             cqdma;      ///< completion queue allocation
    vfio_dma_t*             prplist;    ///< PRP shared list
    int                     prpsize;    ///< PRP size per queue entry
    u16                     id;         ///< NVMe queue id
    u16                     cid;        ///< next cid to check and use
    int                     cidcount;   ///< number of pending cids
    int                     desccount;  ///< number of pending descriptors
    u64*                    cidmask;    ///< cid pending bit mask
    unvme_desc_t*           desclist;   ///< use descriptor list
    unvme_desc_t*           descfree;   ///< free descriptor list
    unvme_desc_t*           descnext;   ///< next pending descriptor to process
} unvme_queue_t;

/// open session
typedef struct _unvme_session {
    unvme_ns_t              ns;         ///< namespace info
    int                     id;         ///< session id (same as queues[0] id)
    int                     qcount;     ///< number of queues
    int                     qsize;      ///< queue size
    int                     masksize;   ///< bit mask size
    unvme_queue_t*          queues;     ///< array of queues
    unvme_iomem_t           iomem;      ///< IO allocated memory info
    struct _unvme_session*  prev;       ///< previous session node
    struct _unvme_session*  next;       ///< next session node
} unvme_session_t;

/// device context
typedef struct _unvme_device {
    vfio_device_t*          vfiodev;    ///< vfio device
    nvme_device_t*          nvmedev;    ///< nvme device
    unvme_session_t*        ses;        ///< session list
    int                     numioqs;    ///< total number of IO queues
} unvme_device_t;

#endif  // _UNVME_CORE_H

