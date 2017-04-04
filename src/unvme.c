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
 * @brief UNVMe client library interface functions.
 */

#include "unvme_core.h"

/**
 * Open a client session.
 * @param   pciname     PCI device name (as BB:DD.F format)
 * @param   nsid        namespace id
 * @return  namespace pointer or NULL if error.
 */
const unvme_ns_t* unvme_open(const char* pciname, int nsid)
{
    int b, d, f;
    if (sscanf(pciname, "%x:%x.%x", &b, &d, &f) != 3) {
        ERROR("invalid PCI %s (expect %%x:%%x.%%x format)", pciname);
        return NULL;
    }
    int pci = (b << 16) + (d << 8) + f;

    return unvme_do_open(pci, nsid, 0, 0);
}

/**
 * Open a client session with specified number of IO queues and queue size.
 * @param   pciname     PCI device name (as BB:DD.F format)
 * @param   nsid        namespace id
 * @param   qcount      number of io queues
 * @param   qsize       io queue size
 * @return  namespace pointer or NULL if error.
 */
const unvme_ns_t* unvme_openq(const char* pciname, int nsid, int qcount, int qsize)
{
    if (nsid < 1 || qcount < 0 || qsize < 0 || qsize == 1) {
        ERROR("invalid nsid %d qcount %d or qsize %d", nsid, qcount, qsize);
        return NULL;
    }

    int b, d, f;
    if (sscanf(pciname, "%x:%x.%x", &b, &d, &f) != 3) {
        ERROR("invalid PCI %s (expect %%x:%%x.%%x format)", pciname);
        return NULL;
    }
    int pci = (b << 16) + (d << 8) + f;

    return unvme_do_open(pci, nsid, qcount, qsize);
}

/**
 * Close a client session and delete its contained io queues.
 * @param   ns          namespace handle
 * @return  0 if ok else error code.
 */
int unvme_close(const unvme_ns_t* ns)
{
    return unvme_do_close(ns);
}

/**
 * Allocate an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   size        buffer size
 * @return  the allocated buffer or NULL if failure.
 */
void* unvme_alloc(const unvme_ns_t* ns, u64 size)
{
    return unvme_do_alloc(ns, size);
}

/**
 * Free an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   buf         buffer pointer
 * @return  0 if ok else -1.
 */
int unvme_free(const unvme_ns_t* ns, void* buf)
{
    return unvme_do_free(ns, buf);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_aread(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    return unvme_rw(ns, qid, NVME_CMD_READ, buf, slba, nlb);
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_awrite(const unvme_ns_t* ns, int qid,
                         const void* buf, u64 slba, u32 nlb)
{
    return unvme_rw(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, nlb);
}

/**
 * Poll for completion status of a previous IO submission.
 * If there's no error, the descriptor will be released.
 * @param   iod         IO descriptor
 * @param   timeout     in seconds
 * @return  0 if ok else error status.
 */
int unvme_apoll(unvme_iod_t iod, int timeout)
{
    return unvme_do_poll(iod, timeout);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_read(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    unvme_desc_t* desc = unvme_rw(ns, qid, NVME_CMD_READ, buf, slba, nlb);
    if (desc) {
        sched_yield();
        return unvme_do_poll(desc, UNVME_TIMEOUT);
    }
    return -1;
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_write(const unvme_ns_t* ns, int qid,
                const void* buf, u64 slba, u32 nlb)
{
    unvme_desc_t* desc = unvme_rw(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, nlb);
    if (desc) {
        sched_yield();
        return unvme_do_poll(desc, UNVME_TIMEOUT);
    }
    return -1;
}

